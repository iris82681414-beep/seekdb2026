// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
//! Bounded file-format readers for seekdb's native table operator. No SQL executes here.
mod csv;
mod jsonl;
mod parquet_reader;
mod writer;
pub use writer::{writer_open, FileWriter};

use chrono::{NaiveDate, NaiveDateTime};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::File;
use std::io::{Seek, SeekFrom};

const MAX_COLUMNS: usize = 1024;
const MAX_RECORD_BYTES: usize = 16 * 1024 * 1024;

#[cxx::bridge(namespace = "oceanbase::file_sql")]
pub mod ffi {
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum ColumnKind {
        Null,
        Integer,
        Double,
        Boolean,
        Text,
        Date,
        DateTime,
    }
    struct FileColumn {
        name: String,
        kind: ColumnKind,
        nullable: bool,
        scale: i8,
    }
    struct FileValue {
        text: String,
        is_null: bool,
        integer: i64,
        number: f64,
    }
    extern "Rust" {
        type FileWriter;
        fn writer_open(fd: i32, options: &[u8], headers: &[u8]) -> Box<FileWriter>;
        fn writer_status(self: &FileWriter) -> i32;
        fn writer_error(self: &FileWriter) -> &str;
        fn write_field(self: &mut FileWriter, bytes: &[u8], is_null: bool) -> i32;
        fn write_row(self: &mut FileWriter) -> i32;
        fn finish(self: &mut FileWriter) -> i32;
        type FileScan;
        fn scan_open(fd: i32, format: &str, options: &[u8]) -> Box<FileScan>;
        fn scan_restore(fd: i32, definition: &[u8]) -> Box<FileScan>;
        fn status(self: &FileScan) -> i32;
        fn error(self: &FileScan) -> &str;
        fn infer(self: &mut FileScan, max_rows: u32) -> i32;
        fn columns(self: &FileScan) -> Vec<FileColumn>;
        fn definition(self: &FileScan) -> String;
        fn project(self: &mut FileScan, columns: &[u32]) -> i32;
        fn next_row(self: &mut FileScan) -> Vec<FileValue>;
        fn rewind(self: &mut FileScan) -> i32;
    }
}

#[derive(Clone, Debug)]
struct ScanError {
    code: i32,
    message: String,
}
impl ScanError {
    fn invalid(message: impl Into<String>) -> Self {
        Self {
            code: -1,
            message: message.into(),
        }
    }
    fn unsupported(message: impl Into<String>) -> Self {
        Self {
            code: -3,
            message: message.into(),
        }
    }
    fn limit(message: impl Into<String>) -> Self {
        Self {
            code: -4,
            message: message.into(),
        }
    }
}
impl From<std::io::Error> for ScanError {
    fn from(error: std::io::Error) -> Self {
        Self {
            code: -2,
            message: error.to_string(),
        }
    }
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Options {
    header: Option<bool>,
    delimiter: String,
    quote: String,
    escape: String,
    nullstr: String,
    sample_size: i64,
    format: String,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            header: None,
            delimiter: ",".into(),
            quote: "\"".into(),
            escape: "\"".into(),
            nullstr: String::new(),
            sample_size: -1,
            format: "newline_delimited".into(),
        }
    }
}
#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
enum Format {
    Csv,
    Json,
    Parquet,
}
#[derive(Clone)]
struct Cell {
    text: String,
    is_null: bool,
    quoted: bool,
}
#[derive(Clone, Serialize, Deserialize)]
struct Column {
    name: String,
    source: String,
    kind: u8,
    nullable: bool,
    #[serde(default)]
    scale: i8,
}
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Stamp {
    dev: u64,
    inode: u64,
    size: u64,
    modified: i64,
    modified_ns: i64,
    changed: i64,
    changed_ns: i64,
}
#[cfg(unix)]
fn stamp(file: &File) -> Result<Stamp, ScanError> {
    use std::os::unix::fs::MetadataExt;
    let m = file.metadata()?;
    if !m.is_file() {
        return Err(ScanError::invalid("File SQL requires a regular file"));
    }
    Ok(Stamp {
        dev: m.dev(),
        inode: m.ino(),
        size: m.len(),
        modified: m.mtime(),
        modified_ns: m.mtime_nsec(),
        changed: m.ctime(),
        changed_ns: m.ctime_nsec(),
    })
}
#[cfg(not(unix))]
fn stamp(_file: &File) -> Result<Stamp, ScanError> {
    Err(ScanError::unsupported(
        "File SQL is supported on Unix hosts",
    ))
}
#[cfg(unix)]
fn duplicate(fd: i32) -> Result<File, ScanError> {
    use std::os::fd::FromRawFd;
    // fcntl validates even a stale descriptor and sets CLOEXEC atomically. Ownership is transferred only
    // for its newly returned, nonnegative descriptor. The caller retains its fd.
    let owned = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 0) };
    if owned < 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(unsafe { File::from_raw_fd(owned) })
}
#[cfg(not(unix))]
fn duplicate(_fd: i32) -> Result<File, ScanError> {
    Err(ScanError::unsupported(
        "File SQL is supported on Unix hosts",
    ))
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Definition {
    version: u32,
    format: Format,
    options: Options,
    stamp: Stamp,
    columns: Vec<Column>,
}

enum Source {
    Csv {
        reader: csv::CsvReader<File>,
        pending: VecDeque<Vec<Cell>>,
    },
    Json(jsonl::JsonReader<File>),
    Parquet(parquet_reader::ParquetReader),
}
enum Record {
    Cells(Vec<Cell>),
    Named(Vec<(String, Cell)>),
}
impl Source {
    fn next(&mut self) -> Result<Option<Record>, ScanError> {
        match self {
            Self::Csv { reader, pending } => Ok(if let Some(row) = pending.pop_front() {
                Some(Record::Cells(row))
            } else {
                reader.next()?.map(Record::Cells)
            }),
            Self::Json(reader) => Ok(reader.next()?.map(Record::Named)),
            Self::Parquet(reader) => Ok(reader.next()?.map(Record::Cells)),
        }
    }
}

pub struct FileScan {
    file: Option<File>,
    source: Option<Source>,
    format: Format,
    options: Options,
    baseline: Option<Stamp>,
    schema: Vec<Column>,
    mapping: HashMap<String, usize>,
    projection: Vec<u32>,
    rows: u64,
    ready: bool,
    code: i32,
    message: String,
}
impl Default for FileScan {
    fn default() -> Self {
        Self {
            file: None,
            source: None,
            format: Format::Csv,
            options: Options::default(),
            baseline: None,
            schema: Vec::new(),
            mapping: HashMap::new(),
            projection: Vec::new(),
            rows: 0,
            ready: false,
            code: 0,
            message: String::new(),
        }
    }
}
impl FileScan {
    fn fail(&mut self, error: ScanError) -> i32 {
        self.code = error.code;
        self.message = error.message;
        self.code
    }
    fn check(&self) -> Result<(), ScanError> {
        if let (Some(file), Some(baseline)) = (&self.file, &self.baseline) {
            if &stamp(file)? == baseline {
                return Ok(());
            }
            return Err(ScanError::invalid(
                "File changed between schema inference and scan, or during scanning",
            ));
        }
        Err(ScanError::invalid("File scan is not open"))
    }
    fn source(&mut self, reset_schema: bool) -> Result<(), ScanError> {
        let mut file = self
            .file
            .as_ref()
            .ok_or_else(|| ScanError::invalid("File scan is not open"))?
            .try_clone()?;
        file.seek(SeekFrom::Start(0))?;
        self.source = Some(match self.format {
            Format::Csv => {
                let mut reader = csv::CsvReader::new(file, &self.options)?;
                let first = reader
                    .next()?
                    .ok_or_else(|| ScanError::invalid("CSV file has no columns"))?;
                let second = reader.next()?;
                let has_header = self
                    .options
                    .header
                    .unwrap_or_else(|| header_like(&first, second.as_deref()));
                if reset_schema {
                    let names = if has_header {
                        first
                            .iter()
                            .enumerate()
                            .map(|(i, c)| {
                                if c.text.is_empty() {
                                    format!("column{}", i + 1)
                                } else {
                                    c.text.clone()
                                }
                            })
                            .collect()
                    } else {
                        (1..=first.len()).map(|i| format!("column{i}")).collect()
                    };
                    self.schema = unique_columns(names)?;
                }
                let mut pending = VecDeque::new();
                if !has_header {
                    pending.push_back(first);
                }
                if let Some(second) = second {
                    pending.push_back(second);
                }
                Source::Csv { reader, pending }
            }
            Format::Json => Source::Json(jsonl::JsonReader::new(file)),
            Format::Parquet => {
                let reader = parquet_reader::ParquetReader::open(file)?;
                if reset_schema {
                    self.schema = reader.columns().to_vec();
                    self.ready = true;
                }
                Source::Parquet(reader)
            }
        });
        self.mapping = self
            .schema
            .iter()
            .enumerate()
            .map(|(i, c)| (c.source.clone(), i))
            .collect();
        self.check()
    }
    fn initialize(&mut self, fd: i32, format: &str, options: &[u8]) -> Result<(), ScanError> {
        self.format = match format {
            "csv" => Format::Csv,
            "json" | "ndjson" => Format::Json,
            "parquet" => Format::Parquet,
            _ => return Err(ScanError::unsupported("Unknown file format")),
        };
        self.options =
            serde_json::from_slice(options).map_err(|e| ScanError::invalid(e.to_string()))?;
        if self.options.sample_size != -1 {
            return Err(ScanError::unsupported("sample_size only supports -1"));
        }
        if self.format == Format::Json
            && self.options.format != "newline_delimited"
            && self.options.format != "auto"
        {
            return Err(ScanError::unsupported(
                "JSON files require newline_delimited format",
            ));
        }
        let file = duplicate(fd)?;
        self.baseline = Some(stamp(&file)?);
        self.file = Some(file);
        self.source(true)?;
        if self.ready {
            self.projection = (0..self.schema.len() as u32).collect();
        }
        Ok(())
    }
    pub fn status(&self) -> i32 {
        self.code
    }
    pub fn error(&self) -> &str {
        &self.message
    }
    pub fn infer(&mut self, max_rows: u32) -> i32 {
        if self.code < 0 {
            return self.code;
        }
        if self.ready {
            return 1;
        }
        let result = (|| -> Result<bool, ScanError> {
            self.check()?;
            for _ in 0..max_rows.clamp(1, 4096) {
                let row = self
                    .source
                    .as_mut()
                    .ok_or_else(|| ScanError::invalid("No reader"))?
                    .next()?;
                let Some(row) = row else {
                    if self.schema.is_empty() {
                        return Err(ScanError::invalid("File has no columns"));
                    }
                    self.ready = true;
                    self.source(false)?;
                    self.projection = (0..self.schema.len() as u32).collect();
                    return Ok(true);
                };
                match row {
                    Record::Cells(values) => {
                        if values.len() != self.schema.len() {
                            return Err(ScanError::invalid(format!(
                                "CSV record {}: expected {} fields, got {}",
                                self.rows + 1,
                                self.schema.len(),
                                values.len()
                            )));
                        }
                        for (column, value) in self.schema.iter_mut().zip(values) {
                            merge(column, &value, false);
                        }
                    }
                    Record::Named(values) => {
                        let mut seen = HashSet::new();
                        for (name, value) in values {
                            let index = if let Some(index) = self.mapping.get(&name) {
                                *index
                            } else {
                                if self.schema.len() >= MAX_COLUMNS {
                                    return Err(ScanError::limit("Too many JSONL columns"));
                                }
                                let index = self.schema.len();
                                let mut new =
                                    unique_name(&name, self.schema.iter().map(|c| c.name.as_str()));
                                if new.is_empty() {
                                    new = format!("column{}", index + 1);
                                }
                                self.schema.push(Column {
                                    name: new,
                                    source: name.clone(),
                                    kind: 0,
                                    nullable: self.rows != 0,
                                    scale: 0,
                                });
                                self.mapping.insert(name.clone(), index);
                                index
                            };
                            merge(&mut self.schema[index], &value, true);
                            seen.insert(index);
                        }
                        for (index, column) in self.schema.iter_mut().enumerate() {
                            if !seen.contains(&index) {
                                column.nullable = true;
                            }
                        }
                    }
                }
                self.rows += 1;
            }
            self.check()?;
            Ok(false)
        })();
        match result {
            Ok(done) => i32::from(done),
            Err(error) => self.fail(error),
        }
    }
    pub fn columns(&self) -> Vec<ffi::FileColumn> {
        self.schema
            .iter()
            .map(|c| ffi::FileColumn {
                name: c.name.clone(),
                kind: to_kind(c.kind),
                nullable: c.nullable || c.kind == 0,
                scale: c.scale,
            })
            .collect()
    }
    pub fn definition(&self) -> String {
        if !self.ready || self.code < 0 {
            return String::new();
        }
        match &self.baseline {
            Some(stamp) => serde_json::to_string(&Definition {
                version: 1,
                format: self.format,
                options: self.options.clone(),
                stamp: stamp.clone(),
                columns: self.schema.clone(),
            })
            .unwrap_or_default(),
            None => String::new(),
        }
    }
    pub fn project(&mut self, columns: &[u32]) -> i32 {
        if self.code < 0 {
            return self.code;
        }
        if !self.ready
            || columns.iter().any(|i| *i as usize >= self.schema.len())
            || columns.iter().collect::<HashSet<_>>().len() != columns.len()
        {
            return self.fail(ScanError::invalid("Invalid file column projection"));
        }
        self.projection = columns.to_vec();
        if let Some(Source::Parquet(reader)) = &mut self.source {
            if let Err(e) = reader.project(columns) {
                return self.fail(e);
            }
        }
        0
    }
    pub fn next_row(&mut self) -> Vec<ffi::FileValue> {
        if self.code != 0 {
            return Vec::new();
        }
        let result = (|| -> Result<Option<Vec<ffi::FileValue>>, ScanError> {
            if !self.ready {
                return Err(ScanError::invalid("Schema inference is not complete"));
            }
            self.check()?;
            let row = self
                .source
                .as_mut()
                .ok_or_else(|| ScanError::invalid("No reader"))?
                .next()?;
            let Some(row) = row else {
                self.check()?;
                return Ok(None);
            };
            let cells = match row {
                Record::Cells(values) => {
                    if values.len() != self.schema.len() {
                        return Err(ScanError::invalid(
                            "File row has a different field count from its schema",
                        ));
                    }
                    values
                }
                Record::Named(values) => {
                    let mut cells = vec![
                        Cell {
                            text: String::new(),
                            is_null: true,
                            quoted: false
                        };
                        self.schema.len()
                    ];
                    for (name, value) in values {
                        let index = *self
                            .mapping
                            .get(&name)
                            .ok_or_else(|| ScanError::invalid("JSONL schema changed"))?;
                        cells[index] = value;
                    }
                    cells
                }
            };
            let output = self
                .projection
                .iter()
                .map(|index| {
                    typed_value(&cells[*index as usize], self.schema[*index as usize].kind)
                })
                .collect::<Result<Vec<_>, _>>()?;
            self.check()?;
            Ok(Some(output))
        })();
        match result {
            Ok(Some(values)) => values,
            Ok(None) => {
                self.code = 1;
                Vec::new()
            }
            Err(e) => {
                self.fail(e);
                Vec::new()
            }
        }
    }
    pub fn rewind(&mut self) -> i32 {
        if self.code < 0 {
            return self.code;
        }
        self.code = 0;
        if let Err(e) = self.check().and_then(|()| self.source(false)) {
            return self.fail(e);
        }
        let projection = self.projection.clone();
        self.project(&projection)
    }
}

pub fn scan_open(fd: i32, format: &str, options: &[u8]) -> Box<FileScan> {
    let mut scan = Box::<FileScan>::default();
    if let Err(error) = scan.initialize(fd, format, options) {
        scan.fail(error);
    }
    scan
}
pub fn scan_restore(fd: i32, definition: &[u8]) -> Box<FileScan> {
    let mut scan = Box::<FileScan>::default();
    let result = (|| -> Result<(), ScanError> {
        if definition.len() > 2 * 1024 * 1024 {
            return Err(ScanError::limit("File definition exceeds byte limit"));
        }
        let def: Definition =
            serde_json::from_slice(definition).map_err(|e| ScanError::invalid(e.to_string()))?;
        if def.version != 1
            || def.columns.is_empty()
            || def.columns.len() > MAX_COLUMNS
            || def
                .columns
                .iter()
                .any(|c| c.kind > 6 || c.name.is_empty() || c.name.len() > 256)
        {
            return Err(ScanError::invalid("Invalid file schema definition"));
        }
        let file = duplicate(fd)?;
        if stamp(&file)? != def.stamp {
            return Err(ScanError::invalid("File changed after schema inference"));
        }
        scan.file = Some(file);
        scan.baseline = Some(def.stamp);
        scan.format = def.format;
        scan.options = def.options;
        scan.schema = def.columns;
        scan.ready = true;
        scan.source(false)?;
        scan.projection = (0..scan.schema.len() as u32).collect();
        Ok(())
    })();
    if let Err(error) = result {
        scan.fail(error);
    }
    scan
}
fn to_kind(kind: u8) -> ffi::ColumnKind {
    match kind {
        1 => ffi::ColumnKind::Integer,
        2 => ffi::ColumnKind::Double,
        3 => ffi::ColumnKind::Boolean,
        4 => ffi::ColumnKind::Text,
        5 => ffi::ColumnKind::Date,
        6 => ffi::ColumnKind::DateTime,
        _ => ffi::ColumnKind::Null,
    }
}
fn type_of(value: &str) -> u8 {
    if value.eq_ignore_ascii_case("true") || value.eq_ignore_ascii_case("false") {
        return 3;
    }
    if value.parse::<i64>().is_ok() {
        return 1;
    }
    if (value.contains('.') || value.contains('e') || value.contains('E'))
        && value.parse::<f64>().is_ok_and(f64::is_finite)
    {
        return 2;
    }
    if value.len() == 10
        && NaiveDate::parse_from_str(value, "%Y-%m-%d").is_ok_and(|d| d.to_string() == value)
    {
        return 5;
    }
    if value.len() >= 19
        && (NaiveDateTime::parse_from_str(value, "%Y-%m-%d %H:%M:%S%.f").is_ok()
            || NaiveDateTime::parse_from_str(value, "%Y-%m-%dT%H:%M:%S%.f").is_ok())
    {
        return 6;
    }
    4
}
fn typed_value(cell: &Cell, kind: u8) -> Result<ffi::FileValue, ScanError> {
    let mut value = ffi::FileValue {
        text: String::new(),
        is_null: cell.is_null,
        integer: 0,
        number: 0.0,
    };
    if cell.is_null {
        return Ok(value);
    }
    let invalid = || ScanError::invalid("File value does not match its inferred type");
    match kind {
        0 => return Err(invalid()),
        1 | 3 => {
            value.integer = if cell.text.eq_ignore_ascii_case("true") {
                1
            } else if cell.text.eq_ignore_ascii_case("false") {
                0
            } else {
                cell.text.parse().map_err(|_| invalid())?
            }
        }
        2 => {
            value.number = if cell.text.eq_ignore_ascii_case("true") {
                1.0
            } else if cell.text.eq_ignore_ascii_case("false") {
                0.0
            } else {
                cell.text.parse().map_err(|_| invalid())?
            };
            if !value.number.is_finite() {
                return Err(invalid());
            }
        }
        5 => {
            let date = NaiveDate::parse_from_str(&cell.text, "%Y-%m-%d").map_err(|_| invalid())?;
            let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).ok_or_else(invalid)?;
            value.integer = date.signed_duration_since(epoch).num_days();
        }
        6 => {
            let dt = NaiveDateTime::parse_from_str(&cell.text, "%Y-%m-%d %H:%M:%S%.f")
                .ok()
                .or_else(|| NaiveDateTime::parse_from_str(&cell.text, "%Y-%m-%dT%H:%M:%S%.f").ok())
                .or_else(|| {
                    NaiveDate::parse_from_str(&cell.text, "%Y-%m-%d")
                        .ok()?
                        .and_hms_opt(0, 0, 0)
                })
                .ok_or_else(invalid)?;
            value.integer = dt.and_utc().timestamp_micros();
        }
        _ => {}
    }
    // Keep the original lexeme for exact VARCHAR widening and diagnostics.
    value.text = cell.text.clone();
    Ok(value)
}

fn merge(column: &mut Column, value: &Cell, json: bool) {
    if value.is_null {
        column.nullable = true;
        return;
    }
    let kind = if json && value.quoted {
        4
    } else {
        type_of(&value.text)
    };
    if kind == 6 {
        let fractional = value
            .text
            .split_once('.')
            .map_or(0, |(_, fraction)| fraction.len().min(6)) as i8;
        column.scale = column.scale.max(fractional);
    }
    column.kind = match (column.kind, kind) {
        (0, k) => k,
        (a, b) if a == b => a,
        (1 | 3, 1 | 3) => 1,
        (1..=3, 1..=3) => 2,
        (5 | 6, 5 | 6) => 6,
        _ => 4,
    };
}
fn unique_name<'a>(name: &str, names: impl Iterator<Item = &'a str>) -> String {
    let used: HashSet<String> = names.map(str::to_lowercase).collect();
    if !used.contains(&name.to_lowercase()) {
        return name.to_owned();
    }
    let mut suffix = 2;
    loop {
        let value = format!("{name}__{suffix}");
        if !used.contains(&value.to_lowercase()) {
            return value;
        }
        suffix += 1;
    }
}
fn unique_columns(names: Vec<String>) -> Result<Vec<Column>, ScanError> {
    if names.len() > MAX_COLUMNS {
        return Err(ScanError::limit("Too many file columns"));
    }
    let mut columns: Vec<Column> = Vec::new();
    for name in names {
        if name.len() > 256 || name.contains('\0') {
            return Err(ScanError::limit("File column name exceeds length limit"));
        }
        let unique = unique_name(&name, columns.iter().map(|c| c.name.as_str()));
        columns.push(Column {
            name: unique,
            source: name,
            kind: 0,
            nullable: false,
            scale: 0,
        });
    }
    Ok(columns)
}
fn header_like(first: &[Cell], second: Option<&[Cell]>) -> bool {
    if first.iter().all(|c| c.is_null || type_of(&c.text) != 4) {
        return false;
    }
    if let Some(second) = second {
        if first.iter().zip(second).any(|(a, b)| {
            !a.is_null && type_of(&a.text) == 4 && !b.is_null && type_of(&b.text) != 4
        }) {
            return true;
        }
        if first.iter().any(|c| !c.is_null && type_of(&c.text) != 4) {
            return false;
        }
    }
    true
}

#[cfg(all(test, unix))]
mod tests;
