-- substr() counts bytes where length() counts characters, so cutting a
-- multi-byte character gives invalid UTF-8.
SELECT substr('é', 1, 1) AS s;
