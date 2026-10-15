\set category random(1, 100)
\set min_price random(0, 900)
SELECT id, name, price
FROM products
WHERE category_id = :category AND in_stock AND price >= :min_price
ORDER BY price, id
LIMIT 20;
