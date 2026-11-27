-- GROUP BY with the common aggregates: a NULL key is its own group, a
-- NULL value is skipped, and HAVING filters the groups.
CREATE TABLE emp (id INTEGER, dept INTEGER, name VARCHAR, sal DOUBLE);
INSERT INTO emp VALUES (1, 10, 'ann', 100.5), (2, 10, 'bob', 80), (3, 20, 'cy', NULL),
  (4, NULL, 'dee', 70), (5, 30, 'eve', 120), (6, 20, 'fay', 95.25), (7, 10, 'gus', 80);
CREATE TABLE dept (id INTEGER, dname VARCHAR);
INSERT INTO dept VALUES (10, 'sales'), (20, 'ops'), (40, 'legal'), (NULL, 'none');
SELECT dept AS dept, count(*) AS n, count(sal) AS nsal, sum(sal) AS total,
       avg(sal) AS mean, min(name) AS lowest, max(sal) AS top
FROM emp
GROUP BY dept
HAVING count(*) >= 1
ORDER BY dept NULLS FIRST;
