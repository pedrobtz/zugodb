-- TINYINT, SMALLINT and INTEGER at their extremes (INTEGER's lowest is
-- left out: -2^31 is R's NA, a zuslothdb_type_error), with NULLs.
CREATE TABLE t (id INTEGER, a TINYINT, b SMALLINT, c INTEGER);
INSERT INTO t VALUES
  (1, -128, -32768, -2147483647),
  (2, 127, 32767, 2147483647),
  (3, 0, 0, 0),
  (4, NULL, NULL, NULL);
SELECT a, b, c FROM t ORDER BY id;
