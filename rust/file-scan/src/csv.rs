// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.

//! A bounded RFC 4180 record reader. Physical newlines inside quoted fields are data.
use crate::{Cell, Options, ScanError, MAX_COLUMNS, MAX_RECORD_BYTES};
use std::io::{BufRead, BufReader, Read, Seek};

pub struct CsvReader<R> {
    input: BufReader<R>,
    delimiter: u8,
    quote: Option<u8>,
    escape: Option<u8>,
    nullstr: String,
    line: u64,
    record: u64,
}

impl<R: Read + Seek> CsvReader<R> {
    pub fn new(reader: R, options: &Options) -> Result<Self, ScanError> {
        let one = |text: &str, optional: bool| {
            if text.is_empty() && optional {
                return Ok(None);
            }
            if text.len() != 1
                || !text.is_ascii()
                || matches!(text.as_bytes()[0], 0 | b'\n' | b'\r')
            {
                return Err(ScanError::invalid(
                    "CSV delimiter, quote and escape must be single ASCII characters",
                ));
            }
            Ok(Some(text.as_bytes()[0]))
        };
        let delimiter = one(&options.delimiter, false)?.unwrap();
        let quote = one(&options.quote, true)?;
        let escape = one(&options.escape, true)?;
        if quote == Some(delimiter) {
            return Err(ScanError::invalid("CSV quote equals delimiter"));
        }
        let mut input = BufReader::with_capacity(64 * 1024, reader);
        if input.fill_buf()?.starts_with(&[0xef, 0xbb, 0xbf]) {
            input.consume(3);
        }
        Ok(Self {
            input,
            delimiter,
            quote,
            escape,
            nullstr: options.nullstr.clone(),
            line: 1,
            record: 0,
        })
    }

    fn byte(&mut self) -> Result<Option<u8>, ScanError> {
        let buffer = self.input.fill_buf()?;
        let value = buffer.first().copied();
        if value.is_some() {
            self.input.consume(1);
        }
        if value == Some(b'\n') {
            self.line += 1;
        }
        Ok(value)
    }

    fn peek(&mut self) -> Result<Option<u8>, ScanError> {
        Ok(self.input.fill_buf()?.first().copied())
    }

    pub fn next(&mut self) -> Result<Option<Vec<Cell>>, ScanError> {
        let start = self.line;
        let mut fields = Vec::new();
        let mut bytes = Vec::new();
        let mut quoted = false;
        let mut in_quote = false;
        let mut closed = false;
        let mut touched = false;
        let mut consumed = 0_usize;
        loop {
            let next = self.byte()?;
            consumed += usize::from(next.is_some());
            if consumed > MAX_RECORD_BYTES {
                return Err(ScanError::limit(format!(
                    "CSV record starting at line {start} exceeds the record byte limit"
                )));
            }
            let Some(value) = next else {
                if in_quote {
                    return Err(ScanError::invalid(format!(
                        "Unterminated CSV quote at line {start}"
                    )));
                }
                if !touched && fields.is_empty() && bytes.is_empty() {
                    return Ok(None);
                }
                self.push(&mut fields, bytes, quoted, start)?;
                self.record += 1;
                return Ok(Some(fields));
            };
            touched = true;
            if in_quote {
                if self.escape == Some(value) && self.escape != self.quote {
                    if self.peek()? == self.quote || self.peek()? == self.escape {
                        if let Some(escaped) = self.byte()? {
                            bytes.push(escaped);
                            consumed += 1;
                        }
                    } else {
                        bytes.push(value);
                    }
                } else if self.quote == Some(value) {
                    if self.escape == self.quote && self.peek()? == self.quote {
                        self.byte()?;
                        bytes.push(value);
                        consumed += 1;
                    } else {
                        in_quote = false;
                        closed = true;
                    }
                } else {
                    bytes.push(value);
                }
                continue;
            }
            if value == self.delimiter {
                self.push(&mut fields, std::mem::take(&mut bytes), quoted, start)?;
                quoted = false;
                closed = false;
            } else if matches!(value, b'\r' | b'\n') {
                if value == b'\r' && self.peek()? == Some(b'\n') {
                    self.byte()?;
                } else if value == b'\r' {
                    self.line += 1;
                }
                self.push(&mut fields, bytes, quoted, start)?;
                self.record += 1;
                return Ok(Some(fields));
            } else if closed {
                return Err(ScanError::invalid(format!(
                    "Unexpected character after CSV closing quote at line {start}"
                )));
            } else if bytes.is_empty() && self.quote == Some(value) {
                quoted = true;
                in_quote = true;
            } else if self.quote == Some(value) {
                return Err(ScanError::invalid(format!(
                    "Unexpected CSV quote at line {start}"
                )));
            } else {
                bytes.push(value);
            }
        }
    }

    fn push(
        &self,
        fields: &mut Vec<Cell>,
        bytes: Vec<u8>,
        quoted: bool,
        line: u64,
    ) -> Result<(), ScanError> {
        if fields.len() >= MAX_COLUMNS {
            return Err(ScanError::limit("Too many CSV columns"));
        }
        let text = String::from_utf8(bytes)
            .map_err(|_| ScanError::invalid(format!("Invalid CSV UTF-8 at line {line}")))?;
        let is_null = !quoted && text == self.nullstr;
        fields.push(Cell {
            text,
            is_null,
            quoted,
        });
        Ok(())
    }
}
