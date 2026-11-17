-- No rows, and each column keeps its type.
CREATE TABLE t (i INTEGER, d DOUBLE, s VARCHAR, b BOOLEAN, g BIGINT);
SELECT i, d, s, b, g FROM t;
