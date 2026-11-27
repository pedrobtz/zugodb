-- A common table expression used twice.
CREATE TABLE emp (id INTEGER, dept INTEGER, name VARCHAR, sal DOUBLE);
INSERT INTO emp VALUES (1, 10, 'ann', 100.5), (2, 10, 'bob', 80), (3, 20, 'cy', NULL),
  (4, NULL, 'dee', 70), (5, 30, 'eve', 120), (6, 20, 'fay', 95.25), (7, 10, 'gus', 80);
CREATE TABLE dept (id INTEGER, dname VARCHAR);
INSERT INTO dept VALUES (10, 'sales'), (20, 'ops'), (40, 'legal'), (NULL, 'none');
WITH d AS (SELECT dept, sum(sal) AS total FROM emp GROUP BY dept)
SELECT a.dept AS dept, a.total AS total, b.total AS other
FROM d a JOIN d b ON a.dept < b.dept
ORDER BY a.dept, b.dept;
