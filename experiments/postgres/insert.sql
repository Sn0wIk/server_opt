\set category random(1, 100)
\set price_cents random(0, 99999)
\set stock random(0, 4)
INSERT INTO products (category_id, name, price, in_stock)
VALUES (:category, 'Inserted product', :price_cents::numeric / 100, :stock <> 0);
