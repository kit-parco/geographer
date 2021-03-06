Geographer Development
======================

## Update, 04/08/19
The _geographer-dev_ repository is archived and moved to a private repository in gitlab. It was used for developing but now, development is done directly on _geographer_ repository using feature branches and private forks (see below).

## Repositories
Our main repository is called _geographer_ and on Github: [github.com/hu-macsy/geographer](https://github.com/hu-macsy/geographer)
It is open to the public and continuously tested with Travis at \url{https://travis-ci.org/kit-parco/geographer}.

Currently there is a private repository called _geographer-dev_, which contains the current content of _geographer_ and, in addition, a few branches of unfinished development. The old _ParcoRepart_ repository is incompatible and should not be used any more.

Mirroring these, there are also repositories _geographer_ and _geographer-dev_ on the Gitlab instance at [https://git.scc.kit.edu](https://git.scc.kit.edu).
These should not be used for new development, but kept reasonably up to date.
They were created as a way to share private developments with collaborators at KIT without a GitHub account.

## Developing
For bugfixes or development which can be public, create a new branch in the _geographer_ repository and push your changes to it.
If you do not have write access, create a public fork instead.
Then, create a pull request on github and assign someone to review the code. It should only be merged if the unit tests in Travis run without errors. 

For private developments, for example implementations for future papers that should not be public yet, create a private fork of the main geographer repository and develop there.
Regularly merge changes from the main repository into your fork to avoid large merge conflicts in the end.
To do this, call `git pull https://github.com/kit-parco/geographer` in your local repository.

## Publishing Changes
There are two ways to make your changes public:
- Change the status of your entire repository to public, then create a pull request towards _geographer/Dev_ on Github.
- Locally clone _geographer_, create a new branch and check it out, call `git pull <your-repo>` to pull in your changes. Then, push the new branch and create a pull request to the _Dev_ branch.
