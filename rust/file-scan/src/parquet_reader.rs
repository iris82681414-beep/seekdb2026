// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
//! Footer-driven, projected row-group decoding using Apache's format library.
use crate::{Cell, Column, ScanError, MAX_COLUMNS, MAX_RECORD_BYTES};
use chrono::{DateTime, Duration, NaiveDate};
use parquet::{
    basic::{ConvertedType, LogicalType, Repetition, Type as PhysicalType},
    file::{reader::FileReader, serialized_reader::SerializedFileReader},
    record::{reader::RowIter, Field},
    schema::types::Type,
};
use std::{
    collections::{HashMap, HashSet},
    fs::File,
    io::{Read, Seek, SeekFrom},
    sync::Arc,
};

pub struct ParquetReader {
    file: File,
    schema: Vec<Column>,
    root: Arc<Type>,
    timestamps_ns: HashSet<usize>,
    iterator: Option<RowIter<'static>>,
    remaining: i64,
    rows: i64,
    projection: Vec<u32>,
}
fn invalid(error: impl std::fmt::Display) -> ScanError {
    ScanError::invalid(format!("Parquet: {error}"))
}
impl ParquetReader {
    pub fn open(mut file: File) -> Result<Self, ScanError> {
        let size = file.metadata()?.len();
        if size < 12 {
            return Err(invalid("file is too short"));
        }
        file.seek(SeekFrom::End(-8))?;
        let mut footer = [0_u8; 8];
        file.read_exact(&mut footer)?;
        let length = u32::from_le_bytes(footer[..4].try_into().map_err(invalid)?) as u64;
        if &footer[4..] != b"PAR1" || length > 16 * 1024 * 1024 || length > size - 8 {
            return Err(invalid("invalid or oversized Footer"));
        }
        let reader = SerializedFileReader::new(file.try_clone()?).map_err(invalid)?;
        let metadata = reader.metadata();
        let root = metadata.file_metadata().schema_descr().root_schema_ptr();
        if root.get_fields().is_empty() || root.get_fields().len() > MAX_COLUMNS {
            return Err(invalid("invalid column count"));
        }
        let mut schema = Vec::new();
        let mut names = HashSet::new();
        let mut timestamps_ns = HashSet::new();
        for (index, field) in root.get_fields().iter().enumerate() {
            let info = field.get_basic_info();
            if !field.is_primitive()
                || !info.has_repetition()
                || info.repetition() == Repetition::REPEATED
            {
                return Err(ScanError::unsupported(
                    "Parquet requires flat, non-repeated columns",
                ));
            }
            if field.name().is_empty()
                || field.name().len() > 256
                || field.name().contains('\0')
                || !names.insert(field.name().to_lowercase())
            {
                return Err(invalid("duplicate or invalid column name"));
            }
            let converted = info.converted_type();
            let physical = field.get_physical_type();
            // The row API deliberately panics on unsupported converted types.
            // Reject those schemas before constructing any row decoder.
            let allowed = match physical {
                PhysicalType::BOOLEAN => converted == ConvertedType::NONE,
                PhysicalType::INT32 => matches!(
                    converted,
                    ConvertedType::NONE
                        | ConvertedType::INT_8
                        | ConvertedType::INT_16
                        | ConvertedType::INT_32
                        | ConvertedType::UINT_8
                        | ConvertedType::UINT_16
                        | ConvertedType::UINT_32
                        | ConvertedType::DATE
                        | ConvertedType::TIME_MILLIS
                        | ConvertedType::DECIMAL
                ),
                PhysicalType::INT64 => matches!(
                    converted,
                    ConvertedType::NONE
                        | ConvertedType::INT_64
                        | ConvertedType::UINT_64
                        | ConvertedType::TIME_MICROS
                        | ConvertedType::TIMESTAMP_MILLIS
                        | ConvertedType::TIMESTAMP_MICROS
                        | ConvertedType::DECIMAL
                ),
                PhysicalType::INT96 => converted == ConvertedType::NONE,
                PhysicalType::FLOAT | PhysicalType::DOUBLE => converted == ConvertedType::NONE,
                PhysicalType::BYTE_ARRAY => matches!(
                    converted,
                    ConvertedType::NONE
                        | ConvertedType::UTF8
                        | ConvertedType::ENUM
                        | ConvertedType::JSON
                        | ConvertedType::BSON
                        | ConvertedType::DECIMAL
                ),
                PhysicalType::FIXED_LEN_BYTE_ARRAY => {
                    matches!(converted, ConvertedType::NONE | ConvertedType::DECIMAL)
                }
            };
            if !allowed {
                return Err(ScanError::unsupported(
                    "Unsupported Parquet logical column type",
                ));
            }
            if converted == ConvertedType::DECIMAL
                && (field.get_precision() > 76
                    || field.get_precision() < 1
                    || field.get_scale() < 0
                    || field.get_scale() > field.get_precision())
            {
                return Err(invalid("invalid decimal precision or scale"));
            }
            let mut kind = match physical {
                PhysicalType::BOOLEAN => 3,
                PhysicalType::INT32 | PhysicalType::INT64 => 1,
                PhysicalType::FLOAT | PhysicalType::DOUBLE => 2,
                PhysicalType::INT96 => 6,
                _ => 4,
            };
            match converted {
                ConvertedType::DATE => kind = 5,
                ConvertedType::TIMESTAMP_MILLIS | ConvertedType::TIMESTAMP_MICROS => kind = 6,
                ConvertedType::TIME_MILLIS
                | ConvertedType::TIME_MICROS
                | ConvertedType::UINT_64 => kind = 4,
                ConvertedType::DECIMAL => kind = 2,
                _ => {}
            }
            if let Some(LogicalType::Timestamp {
                unit: parquet::basic::TimeUnit::NANOS,
                ..
            }) = info.logical_type_ref()
            {
                kind = 6;
                timestamps_ns.insert(index);
            }
            if matches!(info.logical_type_ref(), Some(LogicalType::Unknown)) {
                kind = 0;
            }
            schema.push(Column {
                name: field.name().to_owned(),
                source: field.name().to_owned(),
                kind,
                nullable: info.repetition() != Repetition::REQUIRED,
                scale: if kind == 6 { 6 } else { 0 },
            });
        }
        let rows = metadata.file_metadata().num_rows();
        let mut sum = 0_i64;
        for group in metadata.row_groups() {
            if group.num_rows() < 0
                || group.total_byte_size() < 0
                || group.total_byte_size() > 512 * 1024 * 1024
            {
                return Err(ScanError::limit(
                    "Parquet row group exceeds the uncompressed byte limit",
                ));
            }
            sum = sum
                .checked_add(group.num_rows())
                .ok_or_else(|| invalid("row count overflow"))?;
            for column in group.columns() {
                if column.num_values() != group.num_rows()
                    || column.compressed_size() < 0
                    || column.compressed_size() as u64 > size
                    || column.uncompressed_size() < 0
                    || column.uncompressed_size() > 512 * 1024 * 1024
                    || column.data_page_offset() < 4
                    || column.dictionary_page_offset().is_some_and(|p| p < 4)
                {
                    return Err(invalid("invalid column chunk bounds or value count"));
                }
            }
        }
        if rows < 0 || sum != rows {
            return Err(invalid("inconsistent Footer row count"));
        }
        let projection = (0..schema.len() as u32).collect();
        Ok(Self {
            file,
            schema,
            root,
            timestamps_ns,
            iterator: Some(reader.into_iter().with_batch_size(64)),
            remaining: rows,
            rows,
            projection,
        })
    }
    pub fn columns(&self) -> &[Column] {
        &self.schema
    }
    pub fn project(&mut self, columns: &[u32]) -> Result<(), ScanError> {
        self.projection = columns.to_vec();
        self.remaining = self.rows;
        if columns.is_empty() {
            self.iterator = None;
            return Ok(());
        }
        let wanted: HashSet<usize> = columns.iter().map(|i| *i as usize).collect();
        let fields = self
            .root
            .get_fields()
            .iter()
            .enumerate()
            .filter(|(i, _)| wanted.contains(i))
            .map(|(_, f)| f.clone())
            .collect();
        let projection = Type::group_type_builder(self.root.name())
            .with_fields(fields)
            .build()
            .map_err(invalid)?;
        let reader = SerializedFileReader::new(self.file.try_clone()?).map_err(invalid)?;
        self.iterator = Some(
            reader
                .into_iter()
                .project(Some(projection))
                .map_err(invalid)?
                .with_batch_size(64),
        );
        Ok(())
    }
    pub fn next(&mut self) -> Result<Option<Vec<Cell>>, ScanError> {
        if self.remaining == 0 {
            return Ok(None);
        }
        let mut values = vec![
            Cell {
                text: String::new(),
                is_null: true,
                quoted: false
            };
            self.schema.len()
        ];
        if let Some(iterator) = &mut self.iterator {
            let row = iterator
                .next()
                .ok_or_else(|| invalid("row data ended before Footer row count"))?
                .map_err(invalid)?;
            let lookup: HashMap<&str, usize> = self
                .schema
                .iter()
                .enumerate()
                .map(|(i, c)| (c.source.as_str(), i))
                .collect();
            let mut bytes = 0_usize;
            for (name, field) in row.get_column_iter() {
                let index = *lookup
                    .get(name.as_str())
                    .ok_or_else(|| invalid("unexpected projected field"))?;
                let text = match field {
                    Field::Null => String::new(),
                    Field::Str(v) => v.clone(),
                    Field::Bytes(v) => String::from_utf8(v.data().to_vec()).map_err(invalid)?,
                    Field::Date(v) => NaiveDate::from_ymd_opt(1970, 1, 1)
                        .and_then(|d| d.checked_add_signed(Duration::days(i64::from(*v))))
                        .ok_or_else(|| invalid("date out of range"))?
                        .to_string(),
                    Field::TimestampMillis(v) => DateTime::from_timestamp_millis(*v)
                        .ok_or_else(|| invalid("timestamp out of range"))?
                        .format("%Y-%m-%d %H:%M:%S%.6f")
                        .to_string(),
                    Field::TimestampMicros(v) => DateTime::from_timestamp_micros(*v)
                        .ok_or_else(|| invalid("timestamp out of range"))?
                        .format("%Y-%m-%d %H:%M:%S%.6f")
                        .to_string(),
                    Field::Long(v) if self.timestamps_ns.contains(&index) => {
                        DateTime::from_timestamp_nanos(*v)
                            .format("%Y-%m-%d %H:%M:%S%.6f")
                            .to_string()
                    }
                    Field::Float(v) if !v.is_finite() => {
                        return Err(invalid("non-finite floating point value"))
                    }
                    Field::Double(v) if !v.is_finite() => {
                        return Err(invalid("non-finite floating point value"))
                    }
                    Field::Group(_) | Field::ListInternal(_) | Field::MapInternal(_) => {
                        return Err(ScanError::unsupported(
                            "Nested Parquet values are not supported",
                        ))
                    }
                    _ => field.to_string(),
                };
                bytes = bytes
                    .checked_add(text.len())
                    .ok_or_else(|| ScanError::limit("Parquet row byte limit"))?;
                if bytes > MAX_RECORD_BYTES {
                    return Err(ScanError::limit("Parquet row exceeds byte limit"));
                }
                values[index] = Cell {
                    text,
                    is_null: matches!(field, Field::Null),
                    quoted: false,
                };
            }
        }
        self.remaining -= 1;
        Ok(Some(values))
    }
}
