# NAME

worktrackage - automatic time tracking based on open windows, focus and idle time


# SYNOPSIS

* `make` to build

* `make install` (as root) to put the binaries into `/usr/local/bin`

* `wtsnap -h` to get help about the snapshot making program

* `wtstats -h` to get help about the script that collects statistics over the snapshots


# DESCRIPTION

This program was born out of annoyance over manual work tracking where you have to remember to start and stop the timer.

With worktrackage, you instead regularly take snapshots of the state of your open windows (application name, title, if they're in focus or not, idle time - not screenshots!) and then later analyze how much time you spent on which tasks by analyzing the data you collected.

It's very similar to [arbtt](http://arbtt.nomeata.de/) in spirit, but not in code. Here's a table for quick comparison:

|                    | **worktrackage**    | **arbtt**              |
| ------------------ | ------------------- | ---------------------- |
| **Language**       | C and bash          | Haskell                |
| **Capture Format** | SQLite              | bespoke binary format  |
| **Analysis**       | SQL                 | bespoke language       |
| **Platforms**      | X11                 | X11, OSX, Windows      |
| **Execution**      | via cron or similar | as a daemon on its own |
| **License**        | MIT                 | GPL                    |

The idea is that you run `wtsnap` in fixed intervals (every minute by default) via cron or something. Each snapshot contains the following information:

* `snapshot_id`: A serial id.

* `timestamp`: UTC timestamp the snapshot was taken, looks like `2021-03-26T21:30:41:41.793Z`.

* `sample_time`: The number of seconds this snapshot accounts for (default 60).

* `idle_time`: Milliseconds since the last user interaction.

* And all of the windows that are open:

    * `snapshot_id`: foreign key to the snapshot the window belongs to.

    * `window_id`: the X window id, a string of digits.

    * `parent_id`: parent window id, if this isn't the root window. If you pass `-B` to wtsnap to exclude windows that don't have any name, class or title information, the parent may not actually be in the database.

    * `depth`: how deep into the tree this window is, the root being 1.

    * `focused`: the depth at which the focused window is in this tree. 0 means no focus, `depth == focused` means this is the focused window, `depth < focused` means that some child window is in focus. For most applications, checking if it's zero or not is probably enough.

    * `name`: application name of the window, if existent.

    * `class`: application class of the window, if existent.

    * `title`: window title (`_NET_WM_NAME` or `WM_NAME`), if existent.

Then you can use this information to classify the windows in each snapshot into tasks and sum up the time taken. The `wtstats` script is what works for me: it takes all snapshots with an idle time less than a minute (by default), picks out the focused windows, tries to classify them into tasks, takes the deepest child from each classified snapshot and then sums up the time taken. But you can of course perform arbitrary queries on the database to your heart's content.

To use `wtstats`, you need to give it a classification file. Look at [wtclass.sql](wtclass.sql) for an example. You can copy this file to `~/.wtclass.sql` or use the `-c` option to pass an explicit path. Run `wtstats -u` to show everything it didn't classify.


# NOTES

The snapshot database may contain sensitive information since your window titles may contain private stuff. Protect the file well.

There's no builtin cleanup of the database, so you'll need to clear it out or rotate it yourself if it gets too large. Make sure to run `VACUUM` after a cleanup to actually shrink the file size.


# LICENSE

MIT license, see [the LICENSE file](LICENSE).


# SEE ALSO

* [arbtt](http://arbtt.nomeata.de/) - the inspiration for this.

* [dwm](https://dwm.suckless.org/) - a window manager that helped with figuring out how to operate X11.

* [sqlite](https://sqlite.org/) - instructions for the SQLite database format.
