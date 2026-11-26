-- Infinity stored in a FLOAT column, and read back (a known difference
-- until libzugodb 0.3.0, whose FLOAT range check refused it).
CREATE TABLE t (f FLOAT);
INSERT INTO t VALUES (CAST('Infinity' AS FLOAT));
SELECT f FROM t;
