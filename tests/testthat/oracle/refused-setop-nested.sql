-- A derived table over UNION ALL of derived set operations.
CREATE TABLE emp (id INTEGER, dept INTEGER, name VARCHAR, sal DOUBLE);
INSERT INTO emp VALUES (1, 10, 'ann', 100.5), (2, 10, 'bob', 80), (3, 20, 'cy', NULL),
  (4, NULL, 'dee', 70), (5, 30, 'eve', 120), (6, 20, 'fay', 95.25), (7, 10, 'gus', 80);
CREATE TABLE dept (id INTEGER, dname VARCHAR);
INSERT INTO dept VALUES (10, 'sales'), (20, 'ops'), (40, 'legal'), (NULL, 'none');
SELECT s.x AS x, s.kind AS kind FROM (
  SELECT dept AS x, 'union' AS kind FROM (SELECT dept FROM emp UNION SELECT id FROM dept) u
  UNION ALL
  SELECT dept, 'intersect' FROM (SELECT dept FROM emp INTERSECT SELECT id FROM dept) i
) s ORDER BY kind, x NULLS FIRST;
