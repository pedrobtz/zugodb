-- A recursive CTE: the first twelve Fibonacci numbers.
WITH RECURSIVE f(n, a, b) AS (
  SELECT 1, 0, 1
  UNION ALL
  SELECT n + 1, b, a + b FROM f WHERE n < 12
)
SELECT n AS n, a AS fib FROM f ORDER BY n;
