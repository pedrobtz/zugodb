-- BLOB with a NUL, bytes that are not UTF-8, empty and NULL (Stage 4).
-- The bytes come from unhex(): SlothDB keeps a literal's \x escapes as
-- text (diff-blob-escape).
CREATE TABLE t (k INTEGER, b BLOB);
INSERT INTO t VALUES
  (1, CAST(unhex('AA0062') AS BLOB)), (2, ''::BLOB), (3, 'abc'::BLOB),
  (4, CAST(unhex('FF') AS BLOB)), (5, NULL);
SELECT b FROM t ORDER BY k;
