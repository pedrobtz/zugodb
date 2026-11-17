-- VARCHAR: empty, ASCII, two-, three- and four-byte UTF-8, NULL.
CREATE TABLE t (id INTEGER, s VARCHAR);
INSERT INTO t VALUES
  (1, ''), (2, 'plain'), (3, 'héllo'), (4, '漢字'), (5, '🙂'), (6, NULL),
  (7, 'quote '' inside');
SELECT s FROM t ORDER BY id;
