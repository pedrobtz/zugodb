-- DATE before 1970, on leap days and at the type's extremes (Stage 4).
CREATE TABLE t (k INTEGER, d DATE);
INSERT INTO t VALUES
  (1, DATE '1969-07-20'), (2, DATE '2024-02-29'), (3, DATE '1970-01-01'),
  (4, DATE '1900-02-28'), (5, DATE '1968-02-29'), (6, DATE '0001-01-01'),
  (7, DATE '9999-12-31'), (8, NULL);
SELECT d FROM t ORDER BY k;
