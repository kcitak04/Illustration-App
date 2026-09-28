package com.coverstudio.app.common

import android.app.Activity
import android.content.Intent
import android.view.View
import com.coverstudio.app.project.ProjectInfo

/** What each studio needs from the editor screen that hosts it. */
interface StudioHost {
    val activity: Activity
    val project: ProjectInfo
    fun markDirty()
    fun setBusy(busy: Boolean)
    fun refreshTopBar()
    /** Launch an activity for a result; [onResult] receives the returned data (null if cancelled). */
    fun launchForResult(intent: Intent, onResult: (Intent?) -> Unit)
    fun switchToPhoto()
    fun showInputSettings()
}

/** Common surface both studios expose to the editor. */
interface Studio {
    val root: View
    val canUndo: Boolean
    val canRedo: Boolean
    fun undo()
    fun redo()
    fun toggleLayers()
    fun showMenu(anchor: View)
    fun onShown()
    fun onHidden()
}
