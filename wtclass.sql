-- This is an example classification file.
-- The content here is plugged directly into an SQLite CASE statement.
-- You'll probably want to look at the name, class and title columns.
-- Have a look at the wtstats script for details on how this works.
-- By default, the classfication file lives at ~/.wtclass.sql.

-- Classification for a work task.
when name = 'code-oss'
and (title like '%wtsnap.c%' or title like '%wtstats%')
then 'Worktracker Development'

-- And a classification for a distraction task.
when class = 'firefox'
and title like '%Hacker News%'
then 'News Reading'

-- When you pass -u to wtstats, show_uncategorized will be true.
when show_uncategorized
and name is not null
and title is not null
then '*** ' || title

-- Anything unclassified will be ignored.
else null
