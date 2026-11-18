-- A BLOB literal's \x escapes: duckdb decodes '\xAA\x00b' to three
-- bytes, SlothDB keeps the nine characters. Counted, so the blob class
-- zuslothdb adds is not what differs.
SELECT octet_length('\xAA\x00b'::BLOB) AS n;
