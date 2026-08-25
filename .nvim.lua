vim.keymap.set("n", "<leader>m", function()
	if vim.fn.has("win32") == 1 then
		vim.opt_local.makeprg = "powershell.exe -command ./build.ps1"
	elseif vim.fn.has("linux") == 1 then
		vim.opt_local.makeprg = "sh ./build.sh"
	else
		assert(false, "unsupported platform")
	end

	vim.cmd("silent make!")
	if #vim.fn.getqflist() > 0 then
		vim.cmd("copen")
	end
end, { desc = "Build winspace" })
