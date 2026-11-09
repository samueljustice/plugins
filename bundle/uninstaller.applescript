-- Uninstall Sammy J's Plugins: the app in the macOS disk image, built by bundle/build-macos.sh with
-- osacompile. It asks, then runs Contents/Resources/uninstall-macos.sh: once as root for the apps,
-- plug-ins and receipts, and once as the user for their own Library (and, if asked, settings and presets).

on run
	set appTitle to "Uninstall Sammy J's Plugins"
	set res to POSIX path of (path to me) & "Contents/Resources/"
	set sh to quoted form of (res & "uninstall-macos.sh")
	set pluginsFile to quoted form of (res & "plugins.txt")
	set userdataFile to quoted form of (res & "userdata.txt")

	set names to do shell script sh & " list " & pluginsFile
	display dialog "This removes Sammy J's Plugins from this Mac:" & return & return & names & return & return & ¬
		"Your sessions and recordings aren't touched." with title appTitle ¬
		buttons {"Cancel", "Uninstall"} default button "Uninstall" cancel button "Cancel" with icon caution

	set answer to button returned of (display dialog ¬
		"Also remove your settings and saved presets?" & return & return & ¬
		"Keep them if you might reinstall later." with title appTitle ¬
		buttons {"Remove Them", "Keep Them"} default button "Keep Them")

	try
		do shell script sh & " system " & pluginsFile with prompt "Uninstall Sammy J's Plugins needs your password to remove the plug-ins." with administrator privileges
		set dataFlag to ""
		if answer is "Remove Them" then set dataFlag to " --data"
		do shell script sh & " user " & pluginsFile & " " & userdataFile & dataFlag
	on error errText number errNum
		if errNum is -128 then return -- cancelled at the password prompt
		display dialog "Something went wrong, so some files may still be installed:" & return & return & errText ¬
			with title appTitle buttons {"OK"} default button "OK" with icon stop
		return
	end try

	display dialog "Sammy J's Plugins are uninstalled. Restart your music apps to update their plug-in lists." ¬
		with title appTitle buttons {"OK"} default button "OK"
end run
