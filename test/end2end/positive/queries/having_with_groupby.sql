SELECT fkey FROM R GROUP BY fkey HAVING COUNT(key) > 1 ORDER BY fkey;
