-- TINYINT arithmetic does not check for overflow.
SELECT CAST(127 AS TINYINT) + CAST(1 AS TINYINT) AS v;
