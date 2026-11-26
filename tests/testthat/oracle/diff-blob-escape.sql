-- A BLOB literal's \x escapes: both decode '\xAA\x00b' to three bytes
-- (since libzugodb 0.3.0), and octet_length() is an INTEGER in zugodb
-- where duckdb's is a BIGINT, a double in R. Counted, so the blob class
-- zugodb adds is not what differs.
SELECT octet_length('\xAA\x00b'::BLOB) AS n;
