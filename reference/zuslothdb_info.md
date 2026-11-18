# Information about the bundled engine

Reports which SlothDB source the package was built from. The engine is
vendored from pedrobtz/slothdb, zuslothdb's fork of upstream SlothDB: an
upstream commit and the changes made on top of it, one commit each. A
database file is tied to this commit.

## Usage

``` r
zuslothdb_info()
```

## Value

A list with elements `engine` (`"SlothDB"`), `repository` and `commit`
(the fork and its full commit hash), `tag` (the fork's tag of that
commit), `upstream` and `upstream_commit` (upstream SlothDB and the
commit the fork is based on), `describe` (the last upstream release
before it, as `"v0.2.7+"`), `changes` (a character vector of the fork's
commit subjects since that commit, oldest first) and `threads` (the
engine's thread cap, an integer).

## Examples

``` r
info <- zuslothdb_info()
info$commit
#> [1] "69917820e317fe8bf4e21dd44baf55d3ba450da8"
info$changes
#>  [1] "Take the Windows branch under MinGW, not only MSVC"            
#>  [2] "Write nothing to stderr, never abort, use no rand()"           
#>  [3] "Refuse remote paths: offline only"                             
#>  [4] "Cap every thread pool with one process-wide limit"             
#>  [5] "Refuse STRFTIME with one argument"                             
#>  [6] "Fix two warnings R CMD check counts"                           
#>  [7] "Add a read-only database mode"                                 
#>  [8] "Set stop flags under the mutex their waiters use"              
#>  [9] "Keep the Parquet decoder's scratch out of thread_local"        
#> [10] "Size validity masks by their holder's capacity"                
#> [11] "Update seven doctests to DATE as days since 1970"              
#> [12] "Make ROLLBACK say that nothing was undone"                     
#> [13] "Refuse Parquet columns the reader cannot decode faithfully"    
#> [14] "Refuse unreadable Parquet columns when bound, not when scanned"
#> [15] "Refuse JSON the readers would misread, and decode \\u escapes" 
#> [16] "Refuse Arrow IPC: SlothDB's Arrow format is its own"           
#> [17] "Refuse the SQLite, Excel and Avro readers"                     
```
