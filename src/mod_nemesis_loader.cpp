/*
 * mod-nemesis loader.
 *
 * AzerothCore looks up a loader symbol derived from the module's folder name: for folder
 * "mod-nemesis" that symbol is exactly "Addmod_nemesisScripts". If you clone the repo under a
 * different folder name, rename this function to match.
 *
 * Released under the MIT License.
 */

void AddNemesisScripts();
void AddNemesisCreatureScripts();
void AddNemesisCommandScripts();

void Addmod_nemesisScripts()
{
    AddNemesisScripts();
    AddNemesisCreatureScripts();
    AddNemesisCommandScripts();
}
