# matrixdb tour — run from the repo root:  ./matrixdb -f examples/tour.sql
# (lines starting with # are comments; everything else is a dot-command or a query)

# --- load two row-aligned datasets: orders (region-index, amount, qty) and regions (key -> name) ---
.load examples/orders.csv region u32 col0 header
.load examples/orders.csv amount u32 col1 header
.load examples/orders.csv qty u32 col2 header
.load examples/regions.csv reg_key u32 col0 header
.load examples/regions.csv reg_name str col1 header
.columns

# --- scalar, grouped, filtered ---
SELECT SUM(amount)
SELECT SUM(amount) GROUP BY region
SELECT SUM(amount) WHERE amount > 200

# --- multi-aggregate (a table), top-N, HAVING, distinct ---
SELECT COUNT(amount), SUM(amount), MAX(amount) GROUP BY region
SELECT SUM(amount) GROUP BY region ORDER BY SUM DESC LIMIT 2
SELECT SUM(amount) GROUP BY region HAVING SUM > 700
SELECT COUNT(DISTINCT region)

# --- join each order to its region name, the join cardinality, and the star query (sum per dimension) ---
SELECT amount, reg_name JOIN region = reg_key LIMIT 5
SELECT COUNT(*) JOIN region = reg_key
SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name
SELECT SUM(amount) JOIN region = reg_key GROUP BY reg_name ORDER BY SUM DESC LIMIT 2

# --- persistence + engine stats ---
.stats
.save examples/tour.db
# reopen later in a FRESH session:  ./matrixdb   then   .open examples/tour.db
.quit
