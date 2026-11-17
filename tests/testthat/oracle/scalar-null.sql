-- A bare NULL is an integer NA, as in duckdb; a typed NULL is NA of its type.
SELECT NULL AS n, CAST(NULL AS INTEGER) AS i, CAST(NULL AS DOUBLE) AS d,
  CAST(NULL AS VARCHAR) AS s, CAST(NULL AS BOOLEAN) AS b;
