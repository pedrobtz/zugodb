-- A cross join, and a join whose condition is not an equality.
CREATE TABLE emp (id INTEGER, dept INTEGER, name VARCHAR, sal DOUBLE);
INSERT INTO emp VALUES (1, 10, 'ann', 100.5), (2, 10, 'bob', 80), (3, 20, 'cy', NULL),
  (4, NULL, 'dee', 70), (5, 30, 'eve', 120), (6, 20, 'fay', 95.25), (7, 10, 'gus', 80);
CREATE TABLE dept (id INTEGER, dname VARCHAR);
INSERT INTO dept VALUES (10, 'sales'), (20, 'ops'), (40, 'legal'), (NULL, 'none');
SELECT e.id AS id, d.id AS did
FROM emp e JOIN dept d ON e.dept < d.id
ORDER BY e.id, d.id;
