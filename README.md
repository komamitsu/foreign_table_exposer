# foreign_table_exposer

This PostgreSQL extension exposes foreign tables like a normal table with rewriting Query tree. Some BI tools can't detect foreign tables since they don't consider them when getting table list from `pg_catalog.pg_class`. With this extension those BI tools can detect foreign tables as well as normal tables.

## Installation

Add a directory of `pg_config` to PATH and build and install it.

```sh
make USE_PGXS=1
make install USE_PGXS=1
```

If you want to build it in a source tree of PostgreSQL, use

```sh
make
make install
```

or

```bash
    pgxnclient install foreign_table_exposer
```

## Setup

Write this line in your `postgresql.conf` and then restart PostgreSQL.

```
shared_preload_libraries = 'foreign_table_exposer'
```

Execute this statement when you want to enable this feature.

```sql
CREATE EXTENSION foreign_table_exposer;
```

## Usage

When you scan `pg_catalog.pg_class` with a predicate like `relkind in ('r', 'v')`, this extension automatically rewrites the query to include `'f'` (foreign table).

```sql
select
  relname, nspname, relkind
from
  pg_catalog.pg_class c,
  pg_catalog.pg_namespace n
where relkind in ('r', 'v') and
  nspname not in ('pg_catalog', 'information_schema', 'pg_toast', 'pg_temp_1') and
  n.oid = relnamespace order by nspname, relname;
```

returns

```
         relname      | nspname | relkind
    ------------------+---------+---------
      normal_tbl      | public  | r
      foreign_tbl     | public  | f
      example_view    | public  | v
      (3 rows)

```
