//////////////////////////////////////////////////////////////////////////
//
// pgAgent - PostgreSQL Tools
//
// Copyright (C) 2002 - 2016 The pgAdmin Development Team
// This software is released under the PostgreSQL Licence
//
// job.cpp - pgAgent job
//
//////////////////////////////////////////////////////////////////////////

#include "pgAgent.h"

#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/process.h>

#include <sys/types.h>

#ifndef __WIN32__
#include <errno.h>
#include <sys/wait.h>
#endif

Job::Job(DBconn *conn, const wxString &jid)
{
	threadConn = conn;
	jobid = jid;
	status = wxT("");

	LogMessage(wxString::Format(_("Starting job: %s"), jobid.c_str()), LOG_DEBUG);

	int rc = threadConn->ExecuteVoid(
	             wxT("UPDATE pgagent.pga_job SET jobagentid=") + backendPid + wxT(", joblastrun=now() ")
	             wxT(" WHERE jobagentid IS NULL AND jobid=") + jobid);

	if (rc == 1)
	{
		DBresult *id = threadConn->Execute(
		                   wxT("SELECT nextval('pgagent.pga_joblog_jlgid_seq') AS id"));
		if (id)
		{
			logid = id->GetString(wxT("id"));

			DBresult *res = threadConn->Execute(
			                    wxT("INSERT INTO pgagent.pga_joblog(jlgid, jlgjobid, jlgstatus) ")
			                    wxT("VALUES (") + logid + wxT(", ") + jobid + wxT(", 'r')"));
			if (res)
			{
				status = wxT("r");
				delete res;
			}
			delete id;
		}
	}
}


Job::~Job()
{
	if (status != wxT(""))
	{
		threadConn->ExecuteVoid(
		    wxT("UPDATE pgagent.pga_joblog ")
		    wxT("   SET jlgstatus='") + status + wxT("', jlgduration=now() - jlgstart ")
		    wxT(" WHERE jlgid=") + logid + wxT(";\n")

		    wxT("UPDATE pgagent.pga_job ")
		    wxT("   SET jobagentid=NULL, jobnextrun=NULL ")
		    wxT(" WHERE jobid=") + jobid
		);
	}
	threadConn->Return();

	LogMessage(wxString::Format(_("Completed job: %s"), jobid.c_str()), LOG_DEBUG);
}


int Job::Execute()
{
	int rc = 0;
	bool succeeded = false;
	DBresult *steps = threadConn->Execute(
	                      wxT("SELECT * ")
	                      wxT("  FROM pgagent.pga_jobstep ")
	                      wxT(" WHERE jstenabled ")
	                      wxT("   AND jstjobid=") + jobid +
	                      wxT(" ORDER BY jstname, jstid"));

	if (!steps)
	{
		status = wxT("i");
		return -1;
	}

	while (steps->HasData())
	{
		DBconn *stepConn;
		wxString jslid, stepid, jpecode, output;

		stepid = steps->GetString(wxT("jstid"));

		DBresult *id = threadConn->Execute(
		                   wxT("SELECT nextval('pgagent.pga_jobsteplog_jslid_seq') AS id"));
		if (id)
		{
			jslid = id->GetString(wxT("id"));
			DBresult *res = threadConn->Execute(
			                    wxT("INSERT INTO pgagent.pga_jobsteplog(jslid, jsljlgid, jsljstid, jslstatus) ")
			                    wxT("SELECT ") + jslid + wxT(", ") + logid + wxT(", ") + stepid + wxT(", 'r'")
			                    wxT("  FROM pgagent.pga_jobstep WHERE jstid=") + stepid);

			if (res)
			{
				rc = res->RowsAffected();
				delete res;
			}
			else
				rc = -1;
		}
		delete id;

		if (rc != 1)
		{
			status = wxT("i");
			return -1;
		}

		switch ((int) steps->GetString(wxT("jstkind"))[0])
		{
			case 's':
			{
				wxString jstdbname = steps->GetString(wxT("jstdbname"));
				wxString jstconnstr = steps->GetString(wxT("jstconnstr"));

				stepConn = DBconn::Get(jstconnstr, jstdbname);
				if (stepConn)
				{
					LogMessage(wxString::Format(_("Executing SQL step %s (part of job %s)"), stepid.c_str(), jobid.c_str()), LOG_DEBUG);
					rc = stepConn->ExecuteVoid(steps->GetString(wxT("jstcode")));
					succeeded = stepConn->LastCommandOk();
					output = stepConn->GetLastError();
					stepConn->Return();
				}
				else
				{
					output = _("Couldn't get a connection to the database!");
					succeeded = false;
				}


				break;
			}
			case 'b':
			{
				// Batch jobs are more complex thank SQL, for obvious reasons...
				LogMessage(wxString::Format(_("Executing batch step %s (part of job %s)"), stepid.c_str(), jobid.c_str()), LOG_DEBUG);

				// Get a temporary filename, then reuse it to create an empty directory.
				wxString dirname = wxFileName::CreateTempFileName(wxT("pga_"));
				if (dirname.Length() == 0)
				{
					output = _("Couldn't get a temporary filename!");
					LogMessage(_("Couldn't get a temporary filename!"), LOG_WARNING);
					rc = -1;
					break;
				}

				;
				if (!wxRemoveFile(dirname))
				{
					output.Printf(_("Couldn't remove temporary file: %s"), dirname.c_str());
					LogMessage(output, LOG_WARNING);
					rc = -1;
					break;
				}

				if (!wxMkdir(dirname, 0700))
				{
					output.Printf(_("Couldn't create temporary directory: %s"), dirname.c_str());
					LogMessage(output, LOG_WARNING);
					rc = -1;
					break;
				}

#ifdef __WIN32__
				wxString filename = dirname + wxT("\\") + jobid + wxT("_") + stepid + wxT(".bat");
				wxString errorFile = dirname + wxT("\\") + jobid + wxT("_") + stepid + wxT("_error.txt");
#else
				wxString filename = dirname + wxT("/") + jobid + wxT("_") + stepid + wxT(".scr");
				wxString errorFile = dirname + wxT("/") + jobid + wxT("_") + stepid + wxT("_error.txt");
#endif

				// Write the script
				wxFile *file = new wxFile();

				if (!file->Create(filename, true, wxS_IRUSR | wxS_IWUSR | wxS_IXUSR))
				{
					output.Printf(_("Couldn't create temporary script file: %s"), filename.c_str());
					LogMessage(output, LOG_WARNING);
					rc = -1;
					break;
				}

				if (!file->Open(filename, wxFile::write))
				{
					output.Printf(_("Couldn't open temporary script file: %s"), filename.c_str());
					LogMessage(output, LOG_WARNING);
					wxRemoveFile(filename);
					wxRmdir(dirname);
					rc = -1;
					break;
				}

				wxString code = steps->GetString(wxT("jstcode"));

				// Cleanup the code. If we're on Windows, we need to make all line ends \r\n,
				// If we're on Unix, we need \n
				code.Replace(wxT("\r\n"), wxT("\n"));
#ifdef __WIN32__
				code.Replace(wxT("\n"), wxT("\r\n"));
#endif

				if (!file->Write(code))
				{
					output.Printf(_("Couldn't write to temporary script file: %s"), filename.c_str());
					LogMessage(output, LOG_WARNING);
					wxRemoveFile(filename);
					wxRmdir(dirname);
					rc = -1;
					break;
				}

				file->Close();
				LogMessage(wxString::Format(_("Executing script file: %s"), filename.c_str()), LOG_DEBUG);

				// freopen function is used to redirect output of stream (stderr in our case)
				// into the specified file.
				FILE *fpError = freopen(errorFile.mb_str(), "w", stderr);

				// Execute the file and capture the output
#ifdef __WIN32__
				// The Windows way
				HANDLE h_script, h_process;
				DWORD dwRead;
				char chBuf[4098];

				h_script = win32_popen_r(filename.wc_str(), h_process);
				if (!h_script)
				{
					output.Printf(_("Couldn't execute script: %s, GetLastError() returned %d, errno = %d"), filename.c_str(), GetLastError(), errno);
					LogMessage(output, LOG_WARNING);
					CloseHandle(h_process);
					rc = -1;
					break;
				}


				// Read output from the child process
				if (h_script)
				{
					for (;;)
					{
						if(!ReadFile(h_script, chBuf, 4096, &dwRead, NULL) || dwRead == 0)
							break;

						chBuf[dwRead] = 0;
						output += wxString::FromAscii(chBuf);
					}
				}


				GetExitCodeProcess(h_process, (LPDWORD)&rc);
				CloseHandle(h_process);
				CloseHandle(h_script);

#else
				// The *nix way.
				FILE *fp_script;
				char buf[4098];

				fp_script = popen(filename.mb_str(wxConvUTF8), "r");
				if (!fp_script)
				{
					output.Printf(_("Couldn't execute script: %s, errno = %d"), filename.c_str(), errno);
					LogMessage(output, LOG_WARNING);
					rc = -1;
					break;
				}


				while(!feof(fp_script))
				{
					if (fgets(buf, 4096, fp_script) != NULL)
						output += wxString::FromAscii(buf);
				}

				rc = pclose(fp_script);

				if (WIFEXITED(rc))
					rc = WEXITSTATUS(rc);
				else
					rc = -1;

#endif

				// set success status for batch runs, be pessimistic by default
				LogMessage(wxString::Format(_("Script return code: %d"), rc), LOG_DEBUG);
				succeeded = ((rc == 0) ? true : false);
				// If output is empty then either script did not return any output
				// or script threw some error into stderr.
				// Check script threw some error into stderr
				if (fpError)
				{
					//fclose(fpError);
					FILE* fpErr = fopen(errorFile.mb_str(), "r");
					if (fpErr)
					{
						char buffer[4098];
						wxString errorMsg = wxEmptyString;
						while (!feof(fpErr))
						{
							if (fgets(buffer, 4096, fpErr) != NULL)
								errorMsg += wxString(buffer, wxConvLibc);
						}

						if (errorMsg != wxEmptyString) {
							wxString errmsg =
								wxString::Format(
										_("Script Error: \n%s\n"),
										errorMsg.c_str());
							LogMessage(errmsg, LOG_WARNING);
							output += wxT("\n") + errmsg;
						}

						fclose(fpErr);
					}
					wxRemoveFile(errorFile);
				}

				// Delete the file/directory. If we fail, don't overwrite the script output in the log, just throw warnings.
				if (!wxRemoveFile(filename))
				{
					LogMessage(wxString::Format(_("Couldn't remove temporary script file: %s"), filename.c_str()), LOG_WARNING);
					wxRmdir(dirname);
					break;
				}

				if (!wxRmdir(dirname))
				{
					LogMessage(wxString::Format(_("Couldn't remove temporary directory: "), dirname.c_str()), LOG_WARNING);
					break;
				}

				break;
			}
			default:
			{
				output = _("Invalid step type!");
				status = wxT("i");
				return -1;
			}
		}

		wxString stepstatus;
		if (succeeded)
			stepstatus = wxT("s");
		else
			stepstatus = steps->GetString(wxT("jstonerror"));

		rc = threadConn->ExecuteVoid(
		         wxT("UPDATE pgagent.pga_jobsteplog ")
		         wxT("   SET jslduration = now() - jslstart, ")
		         wxT("       jslresult = ") + NumToStr(rc) + wxT(", jslstatus = '") + stepstatus + wxT("', ")
		         wxT("       jsloutput = ") + threadConn->qtDbString(output) + wxT(" ")
		         wxT(" WHERE jslid=") + jslid);
		if (rc != 1 || stepstatus == wxT("f"))
		{
			status = wxT("f");
			return -1;
		}
		steps->MoveNext();
	}
	delete steps;

	status = wxT("s");
	return 0;
}



JobThread::JobThread(const wxString &jid)
	: wxThread(wxTHREAD_DETACHED)
{
	LogMessage(wxString::Format(_("Creating job thread for job %s"), jid.c_str()), LOG_DEBUG);

	runnable = false;
	jobid = jid;

	DBconn *threadConn = DBconn::Get(DBconn::GetBasicConnectString(), serviceDBname);
	if (threadConn)
	{
		job = new Job(threadConn, jobid);

		if (job->Runnable())
			runnable = true;
	}
}


JobThread::~JobThread()
{
	LogMessage(wxString::Format(_("Destroying job thread for job %s"), jobid.c_str()), LOG_DEBUG);
	delete job;
}


void *JobThread::Entry()
{
	if (runnable)
	{
		job->Execute();
	}

	return(NULL);
}
