-- NULLs past an engine vector (2048 rows) in a table's row group (#45):
-- 5000 rows, made by a cross join of digits, with a NULL in every column
-- each 997 rows and in the first and last rows of the second vector.
CREATE TABLE d (i INTEGER);
INSERT INTO d VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9);
CREATE TABLE t (k INTEGER, b BOOLEAN, i INTEGER, x DOUBLE, s VARCHAR);
INSERT INTO t
SELECT k,
       CASE WHEN k % 997 = 5 OR k IN (2048, 4095) THEN NULL ELSE k % 2 = 0 END,
       CASE WHEN k % 997 = 5 OR k IN (2048, 4095) THEN NULL ELSE k - 2500 END,
       CASE WHEN k % 997 = 5 OR k IN (2048, 4095) THEN NULL ELSE k / 8.0 END,
       CASE WHEN k % 997 = 5 OR k IN (2048, 4095) THEN NULL ELSE 'row ' || CAST(k AS VARCHAR) || ' with some text' END
FROM (SELECT a.i * 1000 + b.i * 100 + c.i * 10 + e.i AS k
      FROM d AS a CROSS JOIN d AS b CROSS JOIN d AS c CROSS JOIN d AS e
      WHERE a.i < 5) AS g;
SELECT * FROM t ORDER BY k;
