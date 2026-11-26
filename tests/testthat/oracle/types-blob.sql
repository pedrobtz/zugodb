-- BLOB with a NUL, bytes that are not UTF-8, empty and NULL (Stage 4).
-- The bytes come from the literal's \x escapes, which the engine reads
-- as DuckDB's since libzugodb 0.3.0.
CREATE TABLE t (k INTEGER, b BLOB);
INSERT INTO t VALUES
  (1, '\xAA\x00b'::BLOB), (2, ''::BLOB), (3, 'abc'::BLOB),
  (4, '\xFF'::BLOB), (5, NULL);
SELECT b FROM t ORDER BY k;
