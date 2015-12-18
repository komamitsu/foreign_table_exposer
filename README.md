# foreign_table_exposer

This PostgreSQL extension exposes foreign tables like a normal table with rewriting Query tree. Some BI tools can't detect foreign tables since they don't consider them when getting table list from `pg_catalog.pg_class`. With this extension those BI tools can detect foreign tables as well as normal tables.

## Installation

    $ make
    $ sudo make install

or

    $ pgxnclient install foreign_table_exposer

## Setup
Write this line in your `postgresql.conf` and then restart PostgreSQL.

    shared_preload_libraries = 'foreign_table_exposer'

Execute this statement when you want to enable this feature.

    CREATE EXTENSION foreign_table_exposer;
    
## Usage
When you scan `pg_catalog.pg_class` with a predicate like `relkind in ('r', 'v')`, this extension automatically rewrites the query to include `'f'` (foreign table).

    postgres=#
      select
        relname, nspname, relkind
      from
        pg_catalog.pg_class c,
        pg_catalog.pg_namespace n
      where relkind in ('r', 'v') and
        nspname not in ('pg_catalog', 'information_schema', 'pg_toast', 'pg_temp_1') and
        n.oid = relnamespace order by nspname, relname;

         relname      | nspname | relkind
    ------------------+---------+---------
      normal_tbl      | public  | r
      foreign_tbl     | public  | f
      example_view    | public  | v
      (3 rows)

