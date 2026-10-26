// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.

//! Decode scalar fields without allocating nested arrays or whole-file JSON trees.
use crate::{Cell, ScanError, MAX_COLUMNS, MAX_RECORD_BYTES};
use serde::de::{self, DeserializeSeed, MapAccess, Visitor};
use serde_json::value::RawValue;
use std::collections::HashSet;
use std::io::{BufRead, BufReader, Read};

pub struct JsonReader<R> {
    input: BufReader<R>,
    line: u64,
}

impl<R: Read> JsonReader<R> {
    pub fn new(reader: R) -> Self {
        Self {
            input: BufReader::with_capacity(64 * 1024, reader),
            line: 0,
        }
    }

    pub fn next(&mut self) -> Result<Option<Vec<(String, Cell)>>, ScanError> {
        loop {
            let mut line = Vec::new();
            loop {
                let data = self.input.fill_buf()?;
                if data.is_empty() {
                    break;
                }
                let count = data
                    .iter()
                    .position(|x| *x == b'\n')
                    .map_or(data.len(), |p| p + 1);
                if line.len() + count > MAX_RECORD_BYTES {
                    return Err(ScanError::limit("JSONL record exceeds the byte limit"));
                }
                let end = data[count - 1] == b'\n';
                line.extend_from_slice(&data[..count]);
                self.input.consume(count);
                if end {
                    break;
                }
            }
            if line.is_empty() {
                return Ok(None);
            }
            self.line += 1;
            let text = std::str::from_utf8(&line).map_err(|_| {
                ScanError::invalid(format!("Invalid JSONL UTF-8 on line {}", self.line))
            })?;
            let text = if self.line == 1 {
                text.trim_start_matches('\u{feff}')
            } else {
                text
            };
            if text.trim().is_empty() {
                continue;
            }
            let mut parser = serde_json::Deserializer::from_str(text);
            let result = FlatRecord
                .deserialize(&mut parser)
                .and_then(|value| {
                    parser.end()?;
                    Ok(value)
                })
                .map_err(|e| ScanError::invalid(format!("JSONL line {}: {e}", self.line)))?;
            return Ok(Some(result));
        }
    }
}

struct FlatRecord;
impl<'de> DeserializeSeed<'de> for FlatRecord {
    type Value = Vec<(String, Cell)>;
    fn deserialize<D: de::Deserializer<'de>>(
        self,
        deserializer: D,
    ) -> Result<Self::Value, D::Error> {
        deserializer.deserialize_map(FlatRecord)
    }
}
impl<'de> Visitor<'de> for FlatRecord {
    type Value = Vec<(String, Cell)>;
    fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.write_str("one flat JSON object")
    }
    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
        let mut fields = Vec::new();
        let mut keys = HashSet::new();
        while let Some(key) = map.next_key::<String>()? {
            if fields.len() >= MAX_COLUMNS {
                return Err(de::Error::custom("Too many JSON fields"));
            }
            if !keys.insert(key.clone()) {
                return Err(de::Error::custom("Duplicate JSON field"));
            }
            if key.is_empty() || key.len() > 256 {
                return Err(de::Error::custom("Invalid JSON column name length"));
            }
            let raw = map.next_value::<&RawValue>()?.get();
            let cell = match raw.as_bytes()[0] {
                b'{' | b'[' => {
                    return Err(de::Error::custom(
                        "Nested JSON objects and arrays are not supported",
                    ))
                }
                b'"' => Cell {
                    text: serde_json::from_str(raw).map_err(de::Error::custom)?,
                    is_null: false,
                    quoted: true,
                },
                _ => Cell {
                    text: raw.to_owned(),
                    is_null: raw == "null",
                    quoted: false,
                },
            };
            fields.push((key, cell));
        }
        Ok(fields)
    }
}
