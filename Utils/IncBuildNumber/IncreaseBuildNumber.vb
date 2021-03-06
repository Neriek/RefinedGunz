Imports EnvDTE
Imports System.Diagnostics

Public Module IncreaseBuildNumber

    Function GetProjectDir(ByVal FullName)

        Dim proj_path
        proj_path = Split(StrReverse(FullName), "\", -1, 1)

        Dim count
        count = UBound(proj_path)

        Dim full_path
        full_path = ""
        Dim i

        For i = 1 To count
            full_path = full_path & "\" & proj_path(i)
        Next

        GetProjectDir = StrReverse(full_path)

    End Function

    Sub ReplaceText(ByVal objSel As TextSelection, _
    ByVal count As Integer, ByVal incrementby As Integer, _
    ByVal Type As Integer)

        'selection represents the TextSelection object
        'count represents the position of the version number
        'to be incremented
        'incrementally represents a number that will be added
        'to the existing version number

        Dim strTemp As String
        Dim i

        strTemp = ""
        objSel.EndOfLine()


        If Type = 0 Then
            For i = 1 To count
                If strTemp.StartsWith(",") = True Then
                    Exit For
                Else
                    objSel.CharLeft(True, 1)
                    strTemp = objSel.Text
                End If
            Next
            strTemp = strTemp.Remove(0, 1)
            strTemp = strTemp + incrementby
            objSel.Text = "," & strTemp
        Else
            For i = 1 To count
                If strTemp.StartsWith(",") = True Then
                    Exit For
                Else
                    objSel.CharLeft(True, 1)
                    strTemp = objSel.Text
                End If
            Next
            strTemp = strTemp.Remove(0, 1)
            strTemp = strTemp.Remove(strTemp.Length - 1, 1)
            strTemp = strTemp + incrementby
            objSel.Text = "," & strTemp & """"
        End If

    End Sub

    Dim WithEvents bldevents As BuildEvents
    Dim applicationObject As EnvDTE.DTE

    Sub BuildDoneEvents(ByVal Application As Object)
        Dim addInInstance As EnvDTE.AddIn

        applicationObject = CType(Application, EnvDTE.DTE)
        bldevents = CType(applicationObject.Events. _
                          BuildEvents, EnvDTE.BuildEvents)

    End Sub

    Private Sub bldevents_OnBuildDone(ByVal _
                          Scope As EnvDTE.vsBuildScope, _
                          ByVal Action As EnvDTE. _
                          vsBuildAction) Handles _
                          bldevents.OnBuildDone
        'This event will be triggered after every build
        'of a project

        Dim soln As Solution
        soln = applicationObject.Solution

        If soln.SolutionBuild.ActiveConfiguration.Name() <> "Release_publish" Then
            Return
        End If

        'Obtain the full path of the active project
        Dim full_path
        full_path = GetProjectDir(applicationObject.ActiveDocument.Path)

        full_path = full_path & "versionno.h"

        'Open the VersionNo.h file
        Dim doc As Document
        applicationObject.ItemOperations.OpenFile(full_path)

        Dim objDoc As TextDocument

        'Obtain the TextSelection object
        objDoc = applicationObject.ActiveDocument.Object("TextDocument")

        Dim objSel As TextSelection = _
                      applicationObject.ActiveDocument.Selection

        objSel.StartOfDocument()


        'Increment the version information
        ReplaceText(objSel, 5, 1, 0)
        objSel.LineDown()
        objSel.StartOfLine()
        ReplaceText(objSel, 5, 1, 0)
        objSel.LineDown()
        objSel.StartOfLine()
        ReplaceText(objSel, 5, 1, 100)
        objSel.LineDown()
        objSel.StartOfLine()
        ReplaceText(objSel, 5, 1, 100)

        applicationObject.ActiveDocument.Save()
        applicationObject.ActiveDocument.Close()
    End Sub



End Module
