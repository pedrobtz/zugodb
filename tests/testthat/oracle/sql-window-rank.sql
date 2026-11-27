-- Ranking functions over a partition: ties share a rank (bob and gus),
-- and a NULL salary sorts by the default NULL order.
CREATE TABLE emp (id INTEGER, dept INTEGER, name VARCHAR, sal DOUBLE);
INSERT INTO emp VALUES (1, 10, 'ann', 100.5), (2, 10, 'bob', 80), (3, 20, 'cy', NULL),
  (4, NULL, 'dee', 70), (5, 30, 'eve', 120), (6, 20, 'fay', 95.25), (7, 10, 'gus', 80);
CREATE TABLE dept (id INTEGER, dname VARCHAR);
INSERT INTO dept VALUES (10, 'sales'), (20, 'ops'), (40, 'legal'), (NULL, 'none');
SELECT id AS id,
       row_number() OVER (PARTITION BY dept ORDER BY sal DESC, id) AS rn,
       rank() OVER (PARTITION BY dept ORDER BY sal DESC) AS rk,
       dense_rank() OVER (PARTITION BY dept ORDER BY sal DESC) AS drk
FROM emp
ORDER BY id;
