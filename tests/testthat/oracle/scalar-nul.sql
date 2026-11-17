-- chr(0) makes a string with a NUL, which R's strings cannot hold: both
-- engines refuse it.
SELECT chr(0) AS s;
