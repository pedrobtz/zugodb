-- Every scalar type through a set operation, whose rows the engine returns
-- boxed as Values (#41): negatives, extremes and NULLs. The UNION ALL is
-- the outermost statement, as an ORDER BY over it would take another path;
-- both engines keep the branches in order.
SELECT CAST(-1 AS TINYINT) AS ti, CAST(-5 AS SMALLINT) AS si,
       -7 AS i, CAST(-9000000000 AS BIGINT) AS bi,
       CAST(1.5 AS FLOAT) AS f, -2.25 AS d, true AS b, 'a' AS s
UNION ALL
SELECT CAST(-128 AS TINYINT), CAST(-32768 AS SMALLINT),
       -2147483647, CAST(-9007199254740992 AS BIGINT),
       CAST(-3.75 AS FLOAT), 1e300, false, 'é'
UNION ALL
SELECT CAST(127 AS TINYINT), CAST(32767 AS SMALLINT),
       2147483647, CAST(9007199254740992 AS BIGINT),
       CAST(0 AS FLOAT), 0.1, NULL, NULL
UNION ALL
SELECT NULL, NULL, NULL, NULL, NULL, NULL, true, '';
