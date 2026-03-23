You broke the import/export pattern tracks feature.
I tried to import some tracks into an existing project that were exported from a different project and it used the instrument-track's order todecide where to import the notes.
The fact that LMMS relies so heavily on implicit identification of entities such as instruments and tracks is an anti-pattern that we are trying to solve.

The bottom line is that you must have recently made changes to those features we explicitly built the feature to not use the instrument-track's index position for import.
It is supposed to create the missing instruments and then reapply all effects, settings, etc...





The export/import feature was supposed to create anything that was missin during import but at some point you regressed horribly and now it is using the index position of the instrument tracks as IDs which we already fixed in a previous branch.
You clearly butchered all of our orignal work when cherry picking commits into this fork.
The purpose of the lmms-db feaature - LITERALLY THE ENTIRE REASON FOR ITS EXISTANCE - is to suppliment LMMS's short-comings (horrible design anti-patterns such as the use of xml which has no id attribute and relies on imlpicit identification via index postiions and other unreliable fragile garbage)
The lmms-db is supposed to allow storage of project data with all the benfits of actual IDs and associations so that the behaviors and functinoality that we are building into lmms are not limited to faulty implicit identification.
The bottom line is that you regressed. I know that this was not a problem on previous versions of the import/export feature.

When the pattern-track (or any kind of track because they can all be exported) is imported, it is supposed to map the midiclips to the correct instruments - if the instrument does not exist in the project bweing imported into then it should add that instrumwent to the pattern store and then import to it.
Your regressin is literally just importing the notes from the first instrumwent into whatever instrument happens to be the first in the new project.

Fix it.
Ive al;ready tols you that you should not be hard coding these kinds of tings but merely keeping a reference to them wherever they might be necessary.
For example, if something depends on using the order of the instrument tracks, your export logic is suposed to give it an ID, map that ID to the index position, and use that information to convert accordingly during import.
If settings_a are aplpied to instrument-tracks[0], then a join-table entry should be create during import that maps the setting to the instrument-track and the instrument-track should store the index position BUT NEVER USE IT AS AN ID.
That way, after importing the instrument track into another project, in which it will probably not have the same index position that it did in the project it was exported from, the settings can be mapped to the correct instrument track in the new project instead of the one at the original index position.

lmms-db exists to afford the flexability and reliability that LMMS's terrible architecture is lacking.
You need to fix your code. ANYTHING THAT USES AN INDEX POSITION (as in the index of an array) instead of actual ID in lmms MUST be handled as I just described.
Give it an ID, map things that depend on implicit index as crude ID get mapped back to entity on import
