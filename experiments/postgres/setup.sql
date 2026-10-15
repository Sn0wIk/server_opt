CREATE TABLE products (
    id bigserial PRIMARY KEY,
    category_id integer NOT NULL,
    name text NOT NULL,
    price numeric(10,2) NOT NULL,
    in_stock boolean NOT NULL
);

-- Deterministic synthetic catalogue; every category has both stock states.
INSERT INTO products (id, category_id, name, price, in_stock)
SELECT i, 1 + (i * 37 % 100)::integer,
       'Product ' || i || ' ' || md5(i::text),
       (i * 7919 % 100000)::numeric / 100,
       (i / 100 % 5) <> 0
FROM generate_series(1, :rows::bigint) AS item(i);

SELECT setval('products_id_seq', :rows::bigint);
-- A plausible existing index, rather than an entirely unindexed baseline.
CREATE INDEX products_category_idx ON products (category_id);
VACUUM (ANALYZE) products;
