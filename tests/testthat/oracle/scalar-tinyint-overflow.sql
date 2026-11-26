-- TINYINT arithmetic is checked for overflow, as DuckDB's (a known
-- difference until libzugodb 0.3.0, which widened the result to 128).
SELECT CAST(127 AS TINYINT) + CAST(1 AS TINYINT) AS v;
