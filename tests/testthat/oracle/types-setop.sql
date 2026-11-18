-- Stage 4's types through a set operation, whose rows the engine returns
-- boxed as Values (#41). The UNION ALL is the outermost statement.
SELECT DATE '1969-07-20' AS d, TIMESTAMP '1969-12-31 23:59:59.5' AS ts,
       CAST(unhex('AA0062') AS BLOB) AS b
UNION ALL
SELECT DATE '2024-02-29', TIMESTAMP '2024-02-29 12:00:00.000001', ''::BLOB
UNION ALL
SELECT NULL, NULL, NULL;
