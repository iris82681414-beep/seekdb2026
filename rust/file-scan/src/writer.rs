// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
//! CSV encoding for the native COPY sink. Publication and path policy stay in C++.
use crate::{duplicate, ScanError, MAX_COLUMNS, MAX_RECORD_BYTES};
use serde::Deserialize;
use std::fs::File;
use std::io::{BufWriter, Write};

#[derive(Deserialize)]
#[serde(default, deny_unknown_fields)]
struct WriterOptions {
    format: String,
    header: bool,
    delimiter: String,
    quote: String,
    escape: String,
    nullstr: String,
}
impl Default for WriterOptions {
    fn default() -> Self {
        Self {
            format: "csv".into(),
            header: false,
            delimiter: ",".into(),
            quote: "\"".into(),
            escape: "\"".into(),
            nullstr: String::new(),
        }
    }
}
pub struct FileWriter {
    output: Option<BufWriter<File>>,
    delimiter: u8,
    quote: Option<u8>,
    escape: Option<u8>,
    nullstr: Vec<u8>,
    columns: usize,
    fields: usize,
    row: Vec<u8>,
    finished: bool,
    code: i32,
    message: String,
}
impl Default for FileWriter {
    fn default() -> Self {
        Self {
            output: None,
            delimiter: b',',
            quote: Some(b'"'),
            escape: Some(b'"'),
            nullstr: Vec::new(),
            columns: 0,
            fields: 0,
            row: Vec::new(),
            finished: false,
            code: 0,
            message: String::new(),
        }
    }
}
impl FileWriter {
    fn fail(&mut self, error: ScanError) -> i32 {
        self.code = error.code;
        self.message = error.message;
        self.code
    }
    pub fn writer_status(&self) -> i32 {
        self.code
    }
    pub fn writer_error(&self) -> &str {
        &self.message
    }
    fn append(&mut self, bytes: &[u8]) -> Result<(), ScanError> {
        if bytes.len() > MAX_RECORD_BYTES.saturating_sub(self.row.len()) {
            return Err(ScanError::limit(
                "COPY CSV row exceeds the record byte limit",
            ));
        }
        self.row.extend_from_slice(bytes);
        Ok(())
    }
    fn field(&mut self, bytes: &[u8], is_null: bool) -> Result<(), ScanError> {
        if self.finished || self.output.is_none() {
            return Err(ScanError::invalid("COPY writer is closed"));
        }
        if self.fields >= self.columns {
            return Err(ScanError::invalid("Too many COPY fields"));
        }
        if !is_null && std::str::from_utf8(bytes).is_err() {
            return Err(ScanError::invalid("COPY CSV requires UTF-8 field values"));
        }
        if self.fields > 0 {
            self.append(&[self.delimiter])?;
        }
        if is_null {
            let nullstr = self.nullstr.clone();
            self.append(&nullstr)?;
        } else {
            let needs_quote = bytes.is_empty()
                || bytes == self.nullstr
                || bytes.iter().any(|b| {
                    *b == self.delimiter || matches!(*b, b'\r' | b'\n') || self.quote == Some(*b)
                });
            if needs_quote {
                let quote = self
                    .quote
                    .ok_or_else(|| ScanError::invalid("COPY field requires a quote character"))?;
                self.append(&[quote])?;
                for &byte in bytes {
                    if byte == quote || (self.escape != self.quote && self.escape == Some(byte)) {
                        let escape = self.escape.ok_or_else(|| {
                            ScanError::invalid("COPY field requires an escape character")
                        })?;
                        self.append(&[escape])?;
                    }
                    self.append(&[byte])?;
                }
                self.append(&[quote])?;
            } else {
                self.append(bytes)?;
            }
        }
        self.fields += 1;
        Ok(())
    }
    pub fn write_field(&mut self, bytes: &[u8], is_null: bool) -> i32 {
        if self.code < 0 {
            return self.code;
        }
        match self.field(bytes, is_null) {
            Ok(()) => 0,
            Err(error) => self.fail(error),
        }
    }
    fn row(&mut self) -> Result<(), ScanError> {
        if self.finished || self.fields != self.columns {
            return Err(ScanError::invalid("Incomplete COPY row"));
        }
        self.append(b"\n")?;
        self.output
            .as_mut()
            .ok_or_else(|| ScanError::invalid("COPY writer is closed"))?
            .write_all(&self.row)?;
        self.row.clear();
        self.fields = 0;
        Ok(())
    }
    pub fn write_row(&mut self) -> i32 {
        if self.code < 0 {
            return self.code;
        }
        match self.row() {
            Ok(()) => 0,
            Err(error) => self.fail(error),
        }
    }
    pub fn finish(&mut self) -> i32 {
        if self.code < 0 {
            return self.code;
        }
        if self.finished {
            return 0;
        }
        let result = (|| {
            if self.fields != 0 {
                return Err(ScanError::invalid("Incomplete COPY row"));
            }
            let writer = self
                .output
                .as_mut()
                .ok_or_else(|| ScanError::invalid("COPY writer is closed"))?;
            writer.flush()?;
            writer.get_ref().sync_all()?;
            Ok(())
        })();
        match result {
            Ok(()) => {
                self.finished = true;
                0
            }
            Err(error) => self.fail(error),
        }
    }
}
pub fn writer_open(fd: i32, options: &[u8], headers: &[u8]) -> Box<FileWriter> {
    let mut writer = FileWriter::default();
    let result = (|| {
        let options: WriterOptions = serde_json::from_slice(options)
            .map_err(|e| ScanError::invalid(format!("Invalid COPY options: {e}")))?;
        let headers: Vec<String> = serde_json::from_slice(headers)
            .map_err(|e| ScanError::invalid(format!("Invalid COPY headers: {e}")))?;
        if !options.format.eq_ignore_ascii_case("csv") {
            return Err(ScanError::unsupported("COPY supports CSV format"));
        }
        if headers.is_empty() || headers.len() > MAX_COLUMNS {
            return Err(ScanError::invalid("Invalid COPY column count"));
        }
        let character = |text: &str, optional: bool| -> Result<Option<u8>, ScanError> {
            if text.is_empty() && optional {
                return Ok(None);
            }
            if text.len() != 1
                || !text.is_ascii()
                || matches!(text.as_bytes()[0], 0 | b'\r' | b'\n')
            {
                return Err(ScanError::invalid(
                    "COPY delimiter, quote and escape must be single ASCII characters",
                ));
            }
            Ok(Some(text.as_bytes()[0]))
        };
        writer.delimiter = character(&options.delimiter, false)?.unwrap();
        writer.quote = character(&options.quote, true)?;
        writer.escape = character(&options.escape, true)?;
        if writer.quote == Some(writer.delimiter) {
            return Err(ScanError::invalid("COPY quote equals delimiter"));
        }
        if options.nullstr.as_bytes().iter().any(|b| {
            *b == writer.delimiter || matches!(*b, b'\r' | b'\n' | 0) || writer.quote == Some(*b)
        }) {
            return Err(ScanError::invalid(
                "COPY NULL marker cannot contain delimiter, quote or newline",
            ));
        }
        writer.nullstr = options.nullstr.into_bytes();
        writer.columns = headers.len();
        writer.output = Some(BufWriter::with_capacity(64 * 1024, duplicate(fd)?));
        if options.header {
            for header in headers {
                writer.field(header.as_bytes(), false)?;
            }
            writer.row()?;
        }
        Ok(())
    })();
    if let Err(error) = result {
        writer.fail(error);
    }
    Box::new(writer)
}
