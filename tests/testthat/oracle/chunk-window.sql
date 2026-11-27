-- row_number() and lag() over n rows.
-- The table t: i from 1 to 4097, with v and s NULL at rows 2048 and 2049
-- (the last of the first 2048-row chunk and the first of the second) and at
-- 4096 and 4097 (each side of the second boundary). Each branch below runs
-- the operator over exactly n rows, n each side of a chunk boundary.
CREATE TABLE dg AS SELECT * FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) v(d);
CREATE TABLE t AS
SELECT i, CASE WHEN i IN (2048, 2049, 4096, 4097) THEN NULL ELSE i END AS v,
       CASE WHEN i IN (2048, 2049, 4096, 4097) THEN NULL ELSE CAST(i AS VARCHAR) END AS s
FROM (SELECT a.d * 1000 + b.d * 100 + c.d * 10 + e.d + 1 AS i
      FROM dg a, dg b, dg c, dg e) g
WHERE i <= 4097;
SELECT 2047 AS n, count(*) AS rows, sum(rn) AS srn, count(lv) AS nlag, sum(lv) AS slag, count(ls) AS nls
FROM (SELECT row_number() OVER (ORDER BY i) AS rn, lag(v) OVER (ORDER BY i) AS lv,
             lag(s) OVER (ORDER BY i) AS ls FROM t WHERE i <= 2047) w
UNION ALL
SELECT 2048 AS n, count(*) AS rows, sum(rn) AS srn, count(lv) AS nlag, sum(lv) AS slag, count(ls) AS nls
FROM (SELECT row_number() OVER (ORDER BY i) AS rn, lag(v) OVER (ORDER BY i) AS lv,
             lag(s) OVER (ORDER BY i) AS ls FROM t WHERE i <= 2048) w
UNION ALL
SELECT 2049 AS n, count(*) AS rows, sum(rn) AS srn, count(lv) AS nlag, sum(lv) AS slag, count(ls) AS nls
FROM (SELECT row_number() OVER (ORDER BY i) AS rn, lag(v) OVER (ORDER BY i) AS lv,
             lag(s) OVER (ORDER BY i) AS ls FROM t WHERE i <= 2049) w
UNION ALL
SELECT 4095 AS n, count(*) AS rows, sum(rn) AS srn, count(lv) AS nlag, sum(lv) AS slag, count(ls) AS nls
FROM (SELECT row_number() OVER (ORDER BY i) AS rn, lag(v) OVER (ORDER BY i) AS lv,
             lag(s) OVER (ORDER BY i) AS ls FROM t WHERE i <= 4095) w
UNION ALL
SELECT 4096 AS n, count(*) AS rows, sum(rn) AS srn, count(lv) AS nlag, sum(lv) AS slag, count(ls) AS nls
FROM (SELECT row_number() OVER (ORDER BY i) AS rn, lag(v) OVER (ORDER BY i) AS lv,
             lag(s) OVER (ORDER BY i) AS ls FROM t WHERE i <= 4096) w
UNION ALL
SELECT 4097 AS n, count(*) AS rows, sum(rn) AS srn, count(lv) AS nlag, sum(lv) AS slag, count(ls) AS nls
FROM (SELECT row_number() OVER (ORDER BY i) AS rn, lag(v) OVER (ORDER BY i) AS lv,
             lag(s) OVER (ORDER BY i) AS ls FROM t WHERE i <= 4097) w
;
