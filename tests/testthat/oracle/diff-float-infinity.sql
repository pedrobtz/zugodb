-- Infinity stored in a FLOAT column (a SELECT of the same cast agrees).
CREATE TABLE t (f FLOAT);
INSERT INTO t VALUES (CAST('Infinity' AS FLOAT));
SELECT f FROM t;
