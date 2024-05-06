CS 61 Problem Set 3
===================

**Fill out both this file and `AUTHORS.md` before submitting.** We grade
anonymously, so put all personally identifying information, including
collaborators, in `AUTHORS.md`.

Grading notes (if any)
----------------------



Extra credit attempted (if any)
-------------------------------
EXTRA CREDIT HAS INDEED BEEN ATTEMPTED.
1. COW (COPY ON WRITE!). Essentially, I've added a if condition to check for this in the exceptions
that checks if we are calling on parent or child (or if the child has died) and then some logic that essentially just makes sure that we only copy the actual contents when we want to write (there is a new flag that I made that allows this, called PTE_C)
2. Kill: sys_kill has been implemented. Kills the process.
3. TEST FOR SYSKILL: p-kill.cc (basically similar to p-exit but I have made some comments highlighting the new parts of the file)