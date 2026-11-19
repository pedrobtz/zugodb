# Package index

## All functions

- [`dbConnect(`*`<zugodb_driver>`*`)`](https://pedrobtz.github.io/zugodb/reference/dbConnect-zugodb_driver-method.md)
  [`dbIsValid(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/dbConnect-zugodb_driver-method.md)
  [`show(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/dbConnect-zugodb_driver-method.md)
  [`dbGetInfo(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/dbConnect-zugodb_driver-method.md)
  : Connect to a zugodb database
- [`dbDisconnect(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/dbDisconnect-zugodb_connection-method.md)
  : Disconnect from a zugodb database
- [`dbExecute(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/dbExecute-zugodb_connection-character-method.md)
  : Run a statement
- [`zug_info()`](https://pedrobtz.github.io/zugodb/reference/zug_info.md)
  : Information about the bundled engine
- [`zugodb()`](https://pedrobtz.github.io/zugodb/reference/zugodb.md)
  [`show(`*`<zugodb_driver>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb.md)
  [`dbIsValid(`*`<zugodb_driver>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb.md)
  [`dbGetInfo(`*`<zugodb_driver>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb.md)
  : The zugodb driver
- [`dbSendQuery(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbSendStatement(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbFetch(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbClearResult(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbIsValid(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbHasCompleted(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbGetRowCount(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbGetRowsAffected(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbGetStatement(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbColumnInfo(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`dbBind(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  [`show(`*`<zugodb_result>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_result.md)
  : Query results
- [`dbWriteTable(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbWriteTable(`*`<zugodb_connection>`*`,`*`<Id>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbWriteTable(`*`<zugodb_connection>`*`,`*`<SQL>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbAppendTable(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbAppendTable(`*`<zugodb_connection>`*`,`*`<Id>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbAppendTable(`*`<zugodb_connection>`*`,`*`<SQL>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbListTables(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbExistsTable(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbExistsTable(`*`<zugodb_connection>`*`,`*`<Id>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbExistsTable(`*`<zugodb_connection>`*`,`*`<SQL>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbListFields(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbListFields(`*`<zugodb_connection>`*`,`*`<Id>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbListFields(`*`<zugodb_connection>`*`,`*`<SQL>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbRemoveTable(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbRemoveTable(`*`<zugodb_connection>`*`,`*`<Id>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbRemoveTable(`*`<zugodb_connection>`*`,`*`<SQL>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbQuoteIdentifier(`*`<zugodb_connection>`*`,`*`<character>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbDataType(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbDataType(`*`<zugodb_driver>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbBegin(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbCommit(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  [`dbRollback(`*`<zugodb_connection>`*`)`](https://pedrobtz.github.io/zugodb/reference/zugodb_tables.md)
  : Write, list and remove tables
