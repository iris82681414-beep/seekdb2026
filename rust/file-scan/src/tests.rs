// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
use super::*;
use std::io::Write;
use std::os::fd::AsRawFd;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

struct TestFile {
    file: File,
    path: std::path::PathBuf,
}
impl TestFile {
    fn new(bytes: &[u8]) -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "seekdb-file-scan-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let mut file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&path)
            .unwrap();
        file.write_all(bytes).unwrap();
        Self { file, path }
    }
    fn open(&self, format: &str, options: &str) -> Box<FileScan> {
        scan_open(self.file.as_raw_fd(), format, options.as_bytes())
    }
}
impl Drop for TestFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}
fn infer(scan: &mut FileScan) {
    assert_eq!(scan.status(), 0, "{}", scan.error());
    while scan.infer(2) == 0 {}
    assert_eq!(scan.status(), 0, "{}", scan.error());
}
fn values(scan: &mut FileScan) -> Vec<Option<String>> {
    scan.next_row()
        .into_iter()
        .map(|v| if v.is_null { None } else { Some(v.text) })
        .collect()
}
#[test]
fn csv_types_multiline_quotes_and_literal_empty() {
    let file=TestFile::new(b"\xef\xbb\xbfid,name,score,active,day,created,note,id\r\n1,\"\",98.5,true,2026-08-01,2026-08-01 10:11:12,\"hello,\nworld\",101\r\n2,Bob,87,false,,,\"say \"\"hi\"\"\",102\r\n");
    let mut scan = file.open("csv", "{}");
    infer(&mut scan);
    assert_eq!(
        scan.schema.iter().map(|c| c.kind).collect::<Vec<_>>(),
        vec![1, 4, 2, 3, 5, 6, 4, 1]
    );
    assert_eq!(scan.schema[7].name, "id__2");
    assert!(!scan.schema[1].nullable);
    assert!(scan.schema[4].nullable);
    let first = values(&mut scan);
    assert_eq!(first[1], Some(String::new()));
    assert_eq!(first[6].as_deref(), Some("hello,\nworld"));
    let second = values(&mut scan);
    assert_eq!(second[4], None);
    assert_eq!(second[6].as_deref(), Some("say \"hi\""));
    assert!(scan.next_row().is_empty());
    assert_eq!(scan.status(), 1);
    assert_eq!(scan.rewind(), 0);
    assert_eq!(values(&mut scan), first);
}
#[test]
fn csv_headerless_and_type_widening() {
    let file = TestFile::new(
        b"1,true,2024-02-29,9223372036854775807\n2.5,1,not-a-date,9223372036854775808\n",
    );
    let mut scan = file.open("csv", r#"{"header":false}"#);
    infer(&mut scan);
    assert_eq!(
        scan.schema.iter().map(|c| c.kind).collect::<Vec<_>>(),
        vec![2, 1, 4, 4]
    );
    assert_eq!(scan.schema[0].name, "column1");
    assert_eq!(scan.project(&[3, 0]), 0);
    assert_eq!(
        values(&mut scan),
        vec![Some("9223372036854775807".into()), Some("1".into())]
    );
    assert_eq!(
        values(&mut scan),
        vec![Some("9223372036854775808".into()), Some("2.5".into())]
    );
}
#[test]
fn jsonl_union_preserves_string_scalars_and_missing_nulls() {
    let file = TestFile::new(
        b"{\"id\":1,\"name\":\"10\",\"active\":true}\n{\"id\":2,\"score\":9.5,\"active\":null}\n",
    );
    let mut scan = file.open("json", "{}");
    infer(&mut scan);
    assert_eq!(
        scan.schema
            .iter()
            .map(|c| c.source.as_str())
            .collect::<Vec<_>>(),
        vec!["id", "name", "active", "score"]
    );
    assert_eq!(
        scan.schema.iter().map(|c| c.kind).collect::<Vec<_>>(),
        vec![1, 4, 3, 2]
    );
    assert!(scan.schema[1].nullable && scan.schema[2].nullable && scan.schema[3].nullable);
    assert_eq!(
        values(&mut scan),
        vec![
            Some("1".into()),
            Some("10".into()),
            Some("true".into()),
            None
        ]
    );
    assert_eq!(
        values(&mut scan),
        vec![Some("2".into()), None, None, Some("9.5".into())]
    );
}
#[test]
fn invalid_records_and_resource_limits_fail_before_results() {
    for bytes in [
        b"id,v\n1,\"bad\n".as_slice(),
        b"id,v\n1,2,3\n",
        b"id\n\xff\n",
        b"id\n\"x\"oops\n",
    ] {
        let file = TestFile::new(bytes);
        let mut scan = file.open("csv", "{}");
        while scan.status() == 0 && scan.infer(16) == 0 {}
        assert!(scan.status() < 0, "{}", String::from_utf8_lossy(bytes));
        assert!(scan.next_row().is_empty());
    }
    for bytes in [
        b"{\"x\":[1,2]}\n".as_slice(),
        b"{\"x\":{}}\n",
        b"[1,2]\n",
        b"{\"x\":1,\"x\":2}\n",
        b"{\"x\":1} trailing\n",
    ] {
        let file = TestFile::new(bytes);
        let mut scan = file.open("json", "{}");
        while scan.status() == 0 && scan.infer(16) == 0 {}
        assert!(scan.status() < 0);
    }
    let file = TestFile::new(&vec![b'x'; MAX_RECORD_BYTES + 1]);
    let scan = file.open("csv", "{}");
    assert_eq!(scan.status(), -4);
    let file = TestFile::new(b"x\n1\n");
    assert_eq!(file.open("csv", r#"{"sample_size":10}"#).status(), -3);
}
#[test]
fn serialized_definition_revalidates_file_and_owns_its_descriptor() {
    let mut file = TestFile::new(b"id\n1\n2\n");
    let mut scan = file.open("csv", "{}");
    infer(&mut scan);
    let definition = scan.definition();
    let mut restored = scan_restore(file.file.as_raw_fd(), definition.as_bytes());
    assert_eq!(restored.status(), 0);
    assert_eq!(values(&mut restored), vec![Some("1".into())]);
    file.file.seek(SeekFrom::End(0)).unwrap();
    file.file.write_all(b"3\n").unwrap();
    assert!(restored.next_row().is_empty());
    assert!(restored.status() < 0);
    assert!(scan_restore(file.file.as_raw_fd(), definition.as_bytes()).status() < 0);
    assert!(scan_open(-1, "csv", b"{}").status() < 0);
}
#[test]
fn parquet_multiple_row_groups_nulls_projection_and_metadata_count() {
    use parquet::{
        basic::Compression,
        data_type::{ByteArray, ByteArrayType, Int32Type, Int64Type},
        file::{properties::WriterProperties, writer::SerializedFileWriter},
        schema::parser::parse_message_type,
    };
    let file = TestFile::new(b"");
    let schema=Arc::new(parse_message_type("message t { REQUIRED INT64 id; OPTIONAL BINARY name (UTF8); OPTIONAL INT32 day (DATE); }").unwrap());
    let props = Arc::new(
        WriterProperties::builder()
            .set_compression(Compression::SNAPPY)
            .build(),
    );
    let mut writer =
        SerializedFileWriter::new(file.file.try_clone().unwrap(), schema, props).unwrap();
    for batch in 0..2 {
        let mut group = writer.next_row_group().unwrap();
        let mut c = group.next_column().unwrap().unwrap();
        c.typed::<Int64Type>()
            .write_batch(&[batch * 2 + 1, batch * 2 + 2], None, None)
            .unwrap();
        c.close().unwrap();
        let mut c = group.next_column().unwrap().unwrap();
        let names = if batch == 0 {
            vec![ByteArray::from("alpha")]
        } else {
            vec![ByteArray::from("gamma"), ByteArray::from("delta")]
        };
        let levels = if batch == 0 { vec![1, 0] } else { vec![1, 1] };
        c.typed::<ByteArrayType>()
            .write_batch(&names, Some(&levels), None)
            .unwrap();
        c.close().unwrap();
        let mut c = group.next_column().unwrap().unwrap();
        c.typed::<Int32Type>()
            .write_batch(&[0, 1], Some(&[1, 1]), None)
            .unwrap();
        c.close().unwrap();
        group.close().unwrap();
    }
    writer.close().unwrap();
    let mut scan = file.open("parquet", "{}");
    infer(&mut scan);
    assert_eq!(
        scan.schema.iter().map(|c| c.kind).collect::<Vec<_>>(),
        vec![1, 4, 5]
    );
    assert_eq!(scan.project(&[1]), 0);
    assert_eq!(values(&mut scan), vec![Some("alpha".into())]);
    assert_eq!(values(&mut scan), vec![None]);
    assert_eq!(values(&mut scan), vec![Some("gamma".into())]);
    assert_eq!(values(&mut scan), vec![Some("delta".into())]);
    assert!(scan.next_row().is_empty());
    assert_eq!(scan.status(), 1);
    assert_eq!(scan.rewind(), 0);
    assert_eq!(scan.project(&[]), 0);
    for _ in 0..4 {
        assert!(scan.next_row().is_empty());
        assert_eq!(scan.status(), 0);
    }
    assert!(scan.next_row().is_empty());
    assert_eq!(scan.status(), 1);
}

#[test]
fn copy_csv_round_trip_quotes_multiline_empty_and_null_marker() {
    let file = TestFile::new(b"");
    let options = br#"{"header":true,"nullstr":"NULL"}"#;
    let mut writer = writer_open(file.file.as_raw_fd(), options, br#"["id","text"]"#);
    assert_eq!(writer.writer_status(), 0, "{}", writer.writer_error());
    let samples = [
        Some("hello,\n\"world\""),
        None,
        Some(""),
        Some("NULL"),
        Some("中文"),
    ];
    for (i, text) in samples.iter().enumerate() {
        assert_eq!(writer.write_field((i + 1).to_string().as_bytes(), false), 0);
        assert_eq!(
            writer.write_field(text.unwrap_or("").as_bytes(), text.is_none()),
            0
        );
        assert_eq!(writer.write_row(), 0);
    }
    assert_eq!(writer.finish(), 0);
    assert_eq!(writer.finish(), 0);
    let mut scan = file.open("csv", r#"{"header":true,"nullstr":"NULL"}"#);
    infer(&mut scan);
    for (i, text) in samples.iter().enumerate() {
        let row = values(&mut scan);
        assert_eq!(row[0], Some((i + 1).to_string()));
        assert_eq!(row[1].as_deref(), *text);
    }
    assert!(scan.next_row().is_empty());
    assert_eq!(scan.status(), 1);
}

#[test]
fn copy_custom_delimiter_escape_and_sticky_failure() {
    let file = TestFile::new(b"");
    let options = br#"{"delimiter":"|","quote":"'","escape":"\\"}"#;
    let mut writer = writer_open(file.file.as_raw_fd(), options, br#"["value"]"#);
    assert_eq!(writer.writer_status(), 0, "{}", writer.writer_error());
    for text in ["a|b", "it's \\here", "", "plain"] {
        assert_eq!(writer.write_field(text.as_bytes(), false), 0);
        assert_eq!(writer.write_row(), 0);
    }
    assert_eq!(writer.finish(), 0);
    let mut scan = file.open(
        "csv",
        r#"{"header":false,"delimiter":"|","quote":"'","escape":"\\"}"#,
    );
    infer(&mut scan);
    for text in ["a|b", "it's \\here", "", "plain"] {
        assert_eq!(values(&mut scan), vec![Some(text.into())]);
    }
    let bad = TestFile::new(b"");
    let mut writer = writer_open(bad.file.as_raw_fd(), br#"{"quote":""}"#, br#"["value"]"#);
    assert!(writer.write_field(b"a,b", false) < 0);
    assert!(writer.write_row() < 0);
    assert!(writer.finish() < 0);
    let mut writer = writer_open(bad.file.as_raw_fd(), b"{}", br#"["value"]"#);
    assert!(writer.write_field(&[0xff], false) < 0);
    assert!(writer.finish() < 0);
    let mut writer = writer_open(bad.file.as_raw_fd(), b"{}", br#"["value"]"#);
    assert_eq!(writer.write_field(b"ok", false), 0);
    assert!(writer.finish() < 0);
    assert!(writer_open(-1, b"{}", br#"["value"]"#).writer_status() < 0);
}
