-- Compare exact ordered output for 300 probes, including empty result sets.
SELECT md5(string_agg(category || ':' || minimum_price || ':' || matches, E'\n'
                     ORDER BY category, minimum_price))
FROM (
    SELECT category, minimum_price,
           COALESCE((
               SELECT string_agg(id || ':' || name || ':' || price, E'\n' ORDER BY price, id)
               FROM (
                   SELECT id, name, price
                   FROM products
                   WHERE category_id = category AND in_stock AND price >= minimum_price
                   ORDER BY price, id
                   LIMIT 20
               ) AS limited
           ), '') AS matches
    FROM generate_series(1, 100) AS categories(category)
    CROSS JOIN (VALUES (0), (500), (900)) AS thresholds(minimum_price)
) AS probes;
