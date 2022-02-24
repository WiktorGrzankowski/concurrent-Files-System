# concurrent-Files-System
This program simulates a system of files, where operations that don't collide can be
and are performed concurrently.
For example, we can create folders in a folder with path /a/b/ while removing
folders from /a/c/, since there is no direct linkage between them.
This problem is a case of writers-readers, where all operations that
don't change anything are readers, and operations such as creating
and removing files are writers.
Concurrency is achieved via functions and structures from phtread.h 
library, that is a mutex and two conditionals, that are used in
preliminary and final protocols of both readers and writers.
It is a project made for my university course on concurrent programming.
I was given code for err.h/.c, HashMap.h/.c and path_utils.h/.c, 
whereas my work consisted of implementing Tree.h/.c, which are
the core files in the project.
