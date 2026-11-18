-- TIME: SlothDB's casts give midnight for every value (#43), so
-- zuslothdb refuses a TIME column; duckdb returns the times.
CREATE TABLE t (k INTEGER, t TIME);
INSERT INTO t VALUES
  (1, CAST('00:00:00' AS TIME)), (2, CAST('23:59:59.999999' AS TIME)),
  (3, CAST('12:34:56.789' AS TIME)), (4, NULL);
SELECT t FROM t ORDER BY k;
