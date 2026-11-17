-- 5000 rows: more than one engine chunk.
SELECT CAST(i AS INTEGER) AS n, i AS big, CAST(i AS DOUBLE) / 4 AS q,
  CAST(i AS VARCHAR) AS s, i % 7 = 0 AS b
FROM generate_series(1, 5000) AS t(i)
ORDER BY n;
