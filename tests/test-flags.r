# These functions perform several internal 
# tests and call `stop()` on invalid states. 
x <- hdf5lib::c_flags()
y <- hdf5lib::ld_flags()

# Expect a string. Not NA or "".

if (!inherits(x, "character")) stop("x is not a character")
if (!inherits(y, "character")) stop("y is not a character")

if (length(x) != 1) stop("x length is not 1")
if (length(y) != 1) stop("y length is not 1")

if (is.na(x)) stop("x is NA")
if (is.na(y)) stop("y is NA")

if (!nzchar(x)) stop("x is an empty string")
if (!nzchar(y)) stop("y is an empty string")
