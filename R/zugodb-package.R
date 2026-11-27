#' @keywords internal
"_PACKAGE"

## usethis namespace: start
#' @import methods
#' @importFrom DBI dbConnect dbDisconnect dbExecute dbGetInfo dbIsValid
#' @importFrom DBI dbSendQuery dbSendStatement dbFetch dbClearResult
#' @importFrom DBI dbHasCompleted dbGetRowCount dbGetRowsAffected
#' @importFrom DBI dbGetStatement dbColumnInfo dbBind
#' @importFrom DBI dbWriteTable dbAppendTable dbListTables dbListFields
#' @importFrom DBI dbExistsTable dbRemoveTable dbQuoteIdentifier dbDataType
#' @importFrom DBI dbBegin dbCommit dbRollback
#' @importFrom DBI dbWriteTableArrow dbAppendTableArrow dbCreateTableArrow
#' @useDynLib zugodb, .registration = TRUE
## usethis namespace: end
NULL
