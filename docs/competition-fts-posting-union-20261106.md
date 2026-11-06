# Unscored natural-language posting union — 2026-11-06

Parent: `c251fef9334dd42f7ebf2c7aa19692bec46bab53` (the previously evaluated contest version).

For natural-language queries without relevance projection or a pushed-down filter, with at most eight posting streams, maintain the sorted stream heads directly. Advance the streams tied at the previous winning document and copy the next ID into the existing output buffer. Boolean, scored, function-lookup and larger queries retain the existing heap path. The existing relevance collector still enforces minimum-match constraints; it is not bypassed merely because no score is projected. Offset/limit handling and document ID comparison use the existing implementation.

## Correctness

A real Linux release build completed successfully. Thirteen native mysqltest groups passed against frozen expected outputs: posting_union, post_build_sort, ascii_collation_loop, collation_expr, tokenize_null_options, tokenize_lob, tokenize_function, parser_properties, cn_word, basic_dml, simple_query, score_usage and score_usage_predicates.

The appended posting regression uses 1,203 documents and covers duplicate/missing terms, NULL/empty documents, one/eight/ten-term queries, score comparisons, Boolean queries, prepared reuse, DML and transaction rollback. Its expected output was recorded on the retained parent binary and the existing expected prefix was preserved. A direct unscored minimum_should_match > 1 SQL case was not available in the established SQL suite; the implementation therefore retains the original collector and its error propagation for every candidate document.

## Same-machine measurement

The original workload and all five benchmark/scoring files were unchanged. Each version used 20,000 documents, three samples, 3,000 tokenize rounds, 200 query rounds and 30 warmups. An A–candidate–A comparison used the retained parent binary for both controls.

| Category | Relative elapsed-time reduction |
| --- | ---: |
| Build | 0.0392% |
| Tokenize | -1.3369% |
| Query | 7.6979% |
| Equal-category mean | 2.1334% |

All four hit counts remained 8,001 / 11,000 / 7,332 / 20. These are local measurements, not new platform points. This version has not yet been formally evaluated. An earlier experiment that bypassed the collector was rejected during review; its faster timing is not the result claimed here.

Candidate binary SHA-256: `c1967b060d2f62f49575274c5c7634cad32aee721cdfbbcd95d942e90416ef25`.

The original live instance remained running. The comparison instance stopped, its executable link was restored, and all 6,936 baseline product/build files were checked after source restoration. [Build, native validation and A–candidate–A reports](competition-results/20261106-posting-union/).
