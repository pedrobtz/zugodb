-- An expression with no alias: duckdb names the column after the
-- expression, the engine names every such column expr.
SELECT 1 + 1, count(*) FROM (VALUES (1), (2)) v(x);
