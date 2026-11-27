-- A grouped aggregate over no rows: no rows at all.
CREATE TABLE emp (id INTEGER, dept INTEGER, name VARCHAR, sal DOUBLE);
INSERT INTO emp VALUES (1, 10, 'ann', 100.5), (2, 10, 'bob', 80), (3, 20, 'cy', NULL),
  (4, NULL, 'dee', 70), (5, 30, 'eve', 120), (6, 20, 'fay', 95.25), (7, 10, 'gus', 80);
CREATE TABLE dept (id INTEGER, dname VARCHAR);
INSERT INTO dept VALUES (10, 'sales'), (20, 'ops'), (40, 'legal'), (NULL, 'none');
SELECT dept AS dept, count(*) AS n
FROM emp
WHERE id > 100
GROUP BY dept
ORDER BY dept;
