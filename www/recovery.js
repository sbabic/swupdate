/*
filedrag.js - HTML5 File Drag & Drop demonstration
Featured on SitePoint.com
Developed by Craig Buckler (@craigbuckler) of OptimalWorks.net
*/

// getElementById
function $id(id) {
	return document.getElementById(id);
}


// output information
function Output(msg) {
	var m = $id("messages");
	m.innerHTML = msg + m.innerHTML;
}

(function() {


	// file drag hover
	function FileDragHover(e) {
		e.stopPropagation();
		e.preventDefault();
		e.target.className = (e.type == "dragover" ? "hover" : "");
	}


	// file selection
	function FileSelectHandler(e) {

		// cancel event and hover styling
		FileDragHover(e);

		var o = $id("progress");
		while(o.firstChild) o.removeChild(o.firstChild);
		var m = $id("messages");
		m.innerHTML = "";

		// fetch FileList object
		var files = e.target.files || e.dataTransfer.files;

		// process all File objects
		for (var i = 0, f; f = files[i]; i++) {
			ParseFile(f);
			UploadFile(f);
		}

	}


	// output file information
	function ParseFile(file) {

		Output(
			"<p>File information: <strong>" + file.name +
			"</strong> size: <strong>" + file.size +
			"</strong> bytes</p>"
		);
		// display an image
		if (file.type.indexOf("image") == 0) {

		}
	}

	function askstatus(){
		var status;
		var listempty;
		var msg ="";
		var lasterror = 0;
		$.ajax({cache: false, url: "getstatus.json", success: function(data){
			$.each(data, function(key, val) {
				if (key == "Status")
					status = val;
				if (key == "Msg" && val != "" )
					msg = val;
				if (key == "Error")
					lasterror = val;

			});

			if (msg) {
				if (lasterror == 0)
					Output("<p>" + msg + "</p>");
				else
					Output("<p><strong>" + msg + "</strong></p>");
			}
			if (status == "0") {
				var f = $id("upload")
				f.reset();
				return;
			}
			if (msg)
				poll(50);
			else
				poll(500);

		}, dataType: "json"});

	}

	function poll(timer){
		setTimeout(function(){
			askstatus();
		}, timer);
	}

	// upload JPEG files
	function UploadFile(file) {

		var xhr = new XMLHttpRequest();
		if (xhr.upload) {

			// create progress bar
			var o = $id("progress");

			var progress = o.appendChild(document.createElement("p"));

			progress.appendChild(document.createTextNode("upload " + file.name));


			// progress bar
			xhr.upload.addEventListener("progress", function(e) {
				var pc = parseInt(100 - (e.loaded / e.total * 100));
				progress.style.backgroundPosition = pc + "% 0";
			}, false);

			// file received/failed
			xhr.onreadystatechange = function(e) {
				poll(1000);
				if (xhr.readyState == 4) {
					if (xhr.status != 200) {
						progress.className = "failure";
						return;
					}
					progress.className = "success";
				}
			};

			// start upload
			xhr.open("POST", $id("upload").action, true);
			xhr.setRequestHeader("X_FILENAME", file.name);
			xhr.send(file);

		}

	}


	// initialize
	function Init() {

		var fileselect = $id("fileselect");

		// file select
		fileselect.addEventListener("change", FileSelectHandler, false);

		// is XHR2 available?
		var xhr = new XMLHttpRequest();
	}

	// call initialization file
	if (window.File && window.FileList && window.FileReader) {
		Init();
	}


})();

function postUpdateCommand() {
	var code = 0;
	var error = "";
	var detail = "";
	$.post("postUpdateCommand", {}, function(data){
		$.each(data, function(key, val) {
			if (key == "code")
				code = val;
			else if (key == "error")
				error = val;
			else if (key == "detail")
				detail = val;
		});

		switch (code) {
		case 200:
			Output("<p>Reboot started</p>");
			break;
		case 0:
			Output("<p><strong>no response</strong></p>");
			break;
		default:
			Output("<p><strong>" + error + ": " + detail + "</strong></p>");
		break;
		}

	});
}
