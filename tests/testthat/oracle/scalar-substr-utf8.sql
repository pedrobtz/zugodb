-- substr() counts characters, as length() does (a known difference until
-- libzugodb 0.3.0, which cut a two-byte character in half).
SELECT substr('é', 1, 1) AS s;
