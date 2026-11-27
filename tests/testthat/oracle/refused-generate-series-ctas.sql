-- CREATE TABLE AS over a table function.
CREATE TABLE g AS SELECT i FROM generate_series(1, 3) t(i);
SELECT i AS i FROM g ORDER BY i;
